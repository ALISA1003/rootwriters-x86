#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/ftrace.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/namei.h>
#include <linux/version.h>
#include <linux/kprobes.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Root writers access control module");

/* Поддержка структур ftrace для разных версий ядра */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define FTRACE_REGS struct ftrace_regs
#else
#define FTRACE_REGS struct pt_regs
#endif

#define CONFIG_PATH "/etc/fsc/rootwriters"
#define MAX_UIDS 1024

/* Кэш состояния файла конфигурации */
static struct timespec64 cached_mtime = {0, 0};
static loff_t cached_size = -1; /* Добавлена проверка размера для обхода багов VFS */
static bool cached_absent = true;
static int allowed_uids[MAX_UIDS];
static int num_allowed_uids = 0;
static DEFINE_RWLOCK(uids_lock);

/* Поиск адреса функции (через kprobes для ядер >= 5.7) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
static unsigned long lookup_name(const char *name) {
    struct kprobe kp = { .symbol_name = name };
    unsigned long addr;
    if (register_kprobe(&kp) < 0) return 0;
    addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);
    return addr;
}
#else
static unsigned long lookup_name(const char *name) {
    return kallsyms_lookup_name(name);
}
#endif

/* 
 * Чтение файла с пользователями.
 * Используем kmalloc для массивов, чтобы не переполнять стек ядра
 */
static void update_rootwriters_list_if_needed(void) {
    struct file *f;
    struct inode *inode;
    struct timespec64 current_mtime;
    loff_t current_size;
    char *buf, *p, *line;
    int *new_uids;
    int new_num = 0;
    const struct cred *old_cred;
    struct cred *new_cred;
    int err;

    /* Временно повышаем привилегии до root для чтения конфига */
    new_cred = prepare_creds();
    if (!new_cred) return;
    new_cred->uid = GLOBAL_ROOT_UID;
    new_cred->euid = GLOBAL_ROOT_UID;
    new_cred->fsuid = GLOBAL_ROOT_UID;
    old_cred = override_creds(new_cred);

    f = filp_open(CONFIG_PATH, O_RDONLY, 0);
    if (IS_ERR(f)) {
        err = PTR_ERR(f);
        revert_creds(old_cred);
        put_cred(new_cred);

        if (err == -ENOENT) {
            read_lock(&uids_lock);
            bool currently_absent = cached_absent;
            read_unlock(&uids_lock);
            
            if (!currently_absent) {
                write_lock(&uids_lock);
                cached_absent = true;
                cached_size = -1; /* Сбрасываем размер */
                num_allowed_uids = 0;
                write_unlock(&uids_lock);
            }
        }
        return;
    }
    
    inode = file_inode(f);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    current_mtime = inode_get_mtime(inode);
#else
    current_mtime = inode->i_mtime;
#endif
    current_size = i_size_read(inode); /* Читаем текущий размер файла */

    read_lock(&uids_lock);
    /* Проверяем не только время, но и размер файла! */
    bool need_update = cached_absent || 
                       current_size != cached_size || 
                       timespec64_compare(&current_mtime, &cached_mtime) != 0;
    read_unlock(&uids_lock);

    if (need_update) {
        buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
        new_uids = kmalloc_array(MAX_UIDS, sizeof(int), GFP_KERNEL);
        
        if (buf && new_uids) {
            loff_t pos = 0;
            ssize_t bytes = kernel_read(f, buf, PAGE_SIZE - 1, &pos);
            if (bytes >= 0) {
                buf[bytes] = '\0';
                p = buf;
                while ((line = strsep(&p, "\n")) != NULL) {
                    char *hash = strchr(line, '#');
                    if (hash) *hash = '\0'; // Обрезаем строку по символу комментария
                    
                    while (*line == ' ' || *line == '\t') line++; // Пропускаем пробелы
                    if (*line == '\0') continue;
                    
                    long uid_val;
                    char *endp;
                    uid_val = simple_strtol(line, &endp, 10);
                    if (endp != line && new_num < MAX_UIDS) {
                        new_uids[new_num++] = (int)uid_val;
                    }
                }
            }
            
            write_lock(&uids_lock);
            cached_absent = false;
            cached_mtime = current_mtime;
            cached_size = current_size; /* Сохраняем новый размер в кэш */
            memcpy(allowed_uids, new_uids, sizeof(int) * new_num);
            num_allowed_uids = new_num;
            write_unlock(&uids_lock);
        }
        if (buf) kfree(buf);
        if (new_uids) kfree(new_uids);
    }
    filp_close(f, NULL);
    revert_creds(old_cred);
    put_cred(new_cred);
}

struct ftrace_hook {
    const char *name;
    void *function;
    void *original;
    unsigned long address;
    struct ftrace_ops ops;
};

static asmlinkage ssize_t (*real_vfs_write)(struct file *file, const char __user *buf, size_t count, loff_t *pos);

static asmlinkage ssize_t hook_vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos) {
    struct inode *inode = file_inode(file);
    
    /* 
     * Важно: Проверяем, что файл принадлежит root (UID=0) И это ОБЫЧНЫЙ файл.
     * S_ISREG гарантирует, что мы не заблокируем вывод в терминал (/dev/tty)
     */
    if (inode->i_uid.val == 0 && S_ISREG(inode->i_mode)) {
        update_rootwriters_list_if_needed();

        read_lock(&uids_lock);
        bool absent = cached_absent;
        int num = num_allowed_uids;
        bool allowed = false;
        
        if (absent) {
            allowed = true; // Нет файла - нет ограничений
        } else if (num == 0) {
            allowed = false; // Пустой файл - запрет для всех
        } else {
            for (int i = 0; i < num; i++) {
                if (allowed_uids[i] == current_uid().val) {
                    allowed = true; // Пользователь найден
                    break;
                }
            }
        }
        read_unlock(&uids_lock);
        
        if (!allowed) {
            return -EACCES; // Выдаем "Permission denied"
        }
    }
    
    return real_vfs_write(file, buf, count, pos);
}

static struct ftrace_hook hook = {
    .name = "vfs_write",
    .function = hook_vfs_write,
    .original = &real_vfs_write,
};

static void notrace fh_ftrace_thunk(unsigned long ip, unsigned long parent_ip, struct ftrace_ops *ops, FTRACE_REGS *fregs) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
    struct pt_regs *regs = ftrace_get_regs(fregs);
#else
    struct pt_regs *regs = fregs;
#endif
    struct ftrace_hook *h = container_of(ops, struct ftrace_hook, ops);
    
    /* Защита от Null-pointer dereference */
    if (!regs) return;

    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long)h->function;
}

static int fh_install_hook(struct ftrace_hook *h) {
    h->address = lookup_name(h->name);
    if (!h->address) {
        pr_err("rootwriters: function %s not found\n", h->name);
        return -ENOENT;
    }
    
    /* Динамическое определение отступа для IBT (endbr64) */
#ifdef CONFIG_X86_64
    if (*((u32 *)h->address) == 0xfa1e0ff3) {
        *((unsigned long*) h->original) = h->address + 9;
    } else {
        *((unsigned long*) h->original) = h->address + 5;
    }
#elif defined(CONFIG_ARM64)
    *((unsigned long*) h->original) = h->address + 4;
#else
    *((unsigned long*) h->original) = h->address;
#endif

    h->ops.func = fh_ftrace_thunk;
    /* Устанавливаем флаги сохранения регистров для работы перехвата */
    h->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_IPMODIFY;
    
    ftrace_set_filter_ip(&h->ops, h->address, 0, 0);
    return register_ftrace_function(&h->ops);
}

static void fh_remove_hook(struct ftrace_hook *h) {
    unregister_ftrace_function(&h->ops);
    ftrace_set_filter_ip(&h->ops, h->address, 1, 0);
}

static int __init rootwriters_init(void) {
    if (fh_install_hook(&hook)) {
        pr_err("rootwriters: Failed to hook vfs_write\n");
        return -EINVAL;
    }
    pr_info("rootwriters: Module loaded securely.\n");
    return 0;
}

static void __exit rootwriters_exit(void) {
    fh_remove_hook(&hook);
    pr_info("rootwriters: Module unloaded.\n");
}

module_init(rootwriters_init);
module_exit(rootwriters_exit);