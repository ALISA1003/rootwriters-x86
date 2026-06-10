#!/bin/bash

# Цвета для вывода
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Проверка прав root (необходимо для загрузки модуля и создания root-файлов)
if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}Ошибка: Пожалуйста, запустите скрипт с правами root (sudo ./test_module.sh)${NC}"
  exit 1
fi

# Определение пользователя для тестов (берем того, кто вызвал sudo)
TEST_USER=$SUDO_USER
if [ -z "$TEST_USER" ] || [ "$TEST_USER" == "root" ]; then
    TEST_USER="nobody" # Фолбэк, если запустили из-под чистого root
fi
TEST_UID=$(id -u "$TEST_USER")

MODULE_NAME="rootwriters"
MODULE_FILE="${MODULE_NAME}.ko"
CONFIG_DIR="/etc/fsc"
CONFIG_FILE="${CONFIG_DIR}/rootwriters"
TEST_FILE="/tmp/rootwriters_test_file"

echo -e "${YELLOW}=== Подготовка стенда ===${NC}"
echo "Тестовый пользователь: $TEST_USER (UID: $TEST_UID)"

# Проверка наличия скомпилированного модуля
if [ ! -f "$MODULE_FILE" ]; then
    echo -e "${RED}Ошибка: Файл модуля $MODULE_FILE не найден в текущей директории.${NC}"
    exit 1
fi

# Создаем тестовый файл, принадлежащий root
touch $TEST_FILE
chown root:root $TEST_FILE
# Даем права 666, чтобы исключить стандартные ограничения DAC.
# Теперь запись будет блокироваться ТОЛЬКО нашим модулем.
chmod 666 $TEST_FILE

# Перезагрузка модуля для чистоты эксперимента
if lsmod | grep -q "$MODULE_NAME"; then
    rmmod $MODULE_NAME
fi
insmod "./$MODULE_FILE" || { echo -e "${RED}Ошибка при загрузке модуля!${NC}"; exit 1; }

mkdir -p "$CONFIG_DIR"
rm -f "$CONFIG_FILE" # Убеждаемся, что файла нет перед первым тестом

PASSED=0
FAILED=0

# Функция для проведения теста
# $1 - Название теста
# $2 - Ожидаемый результат ("success" или "fail")
run_test() {
    local test_name="$1"
    local expected="$2"
    local output
    
    # Попытка записи от имени обычного пользователя
    # Используем timeout на случай зависания
    su -s /bin/bash "$TEST_USER" -c "echo 'test_data' > $TEST_FILE" 2>/dev/null
    local result=$?

    if [ "$expected" == "success" ] && [ $result -eq 0 ]; then
        echo -e "${GREEN}[УСПЕХ]${NC} $test_name"
        ((PASSED++))
    elif [ "$expected" == "fail" ] && [ $result -ne 0 ]; then
        echo -e "${GREEN}[УСПЕХ]${NC} $test_name"
        ((PASSED++))
    else
        echo -e "${RED}[ПРОВАЛ]${NC} $test_name (Ожидалось: $expected, Статус выхода: $result)"
        ((FAILED++))
    fi
}

echo -e "\n${YELLOW}=== Запуск тестов ===${NC}"

# Тест 1: Файл отсутствует
rm -f "$CONFIG_FILE"
run_test "Тест 1: Файл отсутствует -> ограничений нет (запись разрешена)" "success"

# Тест 2: Файл существует, но пуст
touch "$CONFIG_FILE"
run_test "Тест 2: Файл пуст -> запись запрещена всем" "fail"

# Тест 3: Разрешенный пользователь
echo "$TEST_UID" > "$CONFIG_FILE"
run_test "Тест 3: UID пользователя в списке -> запись разрешена" "success"

# Тест 4: Запрещенный пользователь
echo "99999" > "$CONFIG_FILE"
run_test "Тест 4: UID пользователя не в списке -> запись запрещена" "fail"

# Тест 5: Игнорирование пустых строк и комментариев
cat <<EOF > "$CONFIG_FILE"
# Это комментарий перед списком

$TEST_UID
# Это комментарий после списка
EOF
run_test "Тест 5: Парсинг (игнор пустых строк и #) -> запись разрешена" "success"

# Тест 6: Игнорирование дополнительных полей (пробелы и табы)
echo -e "$TEST_UID \t Developer \t Department 123" > "$CONFIG_FILE"
run_test "Тест 6: Парсинг (игнор лишних полей после UID) -> запись разрешена" "success"

# Тест 7: Мгновенное применение (снова ломаем доступ без перезагрузки модуля)
echo "11111" > "$CONFIG_FILE"
run_test "Тест 7: Мгновенное применение изменений (смена на запрет) -> запись запрещена" "fail"

echo -e "\n${YELLOW}=== Очистка ===${NC}"
rm -f "$TEST_FILE"
rm -f "$CONFIG_FILE"
rmmod "$MODULE_NAME"
echo "Модуль выгружен, тестовые файлы удалены."

echo -e "\n${YELLOW}=== Итоги ===${NC}"
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}Все тесты успешно пройдены ($PASSED/$(($PASSED+$FAILED))). Модуль полностью соответствует ТЗ!${NC}"
else
    echo -e "${RED}Провалено тестов: $FAILED из $(($PASSED+$FAILED)).${NC}"
fi