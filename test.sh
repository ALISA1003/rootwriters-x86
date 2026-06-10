#!/bin/bash

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}Запустите через sudo ./test.sh${NC}"
  exit 1
fi

TEST_USER=$SUDO_USER
if [ -z "$TEST_USER" ] || [ "$TEST_USER" == "root" ]; then
    TEST_USER="nobody"
fi
TEST_UID=$(id -u "$TEST_USER")

CONFIG_DIR="/etc/fsc"
CONFIG_FILE="${CONFIG_DIR}/rootwriters"
TEST_FILE="/tmp/rootwriters_test_file"

# Подготовка
mkdir -p "$CONFIG_DIR"
touch $TEST_FILE
chown root:root $TEST_FILE
chmod 666 $TEST_FILE

PASSED=0
FAILED=0

run_test() {
    local test_name="$1"
    local expected="$2"
    
    sudo -u "$TEST_USER" bash -c "echo 'test_data' > $TEST_FILE" 2>/dev/null
    local result=$?

    if [ "$expected" == "success" ] && [ $result -eq 0 ]; then
        echo -e "${GREEN}[УСПЕХ]${NC} $test_name"
        ((PASSED++))
    elif [ "$expected" == "fail" ] && [ $result -ne 0 ]; then
        echo -e "${GREEN}[УСПЕХ]${NC} $test_name"
        ((PASSED++))
    else
        echo -e "${RED}[ПРОВАЛ]${NC} $test_name"
        ((FAILED++))
    fi
}

# Функция безопасного обновления конфига
update_config() {
    local content="$1"
    
    # Искусственная задержка, чтобы mtime гарантированно изменился
    # (компенсирует низкую точность часов виртуальной файловой системы)
    sleep 1.1 
    
    rm -f "$CONFIG_FILE"
    if [ "$content" != "EMPTY" ] && [ "$content" != "ABSENT" ]; then
        echo -e "$content" > /tmp/temp_rw_config
        mv /tmp/temp_rw_config "$CONFIG_FILE"
        chown root:root "$CONFIG_FILE"
    elif [ "$content" == "EMPTY" ]; then
        touch "$CONFIG_FILE"
        chown root:root "$CONFIG_FILE"
    fi
}
echo -e "\n${YELLOW}=== Запуск тестов ===${NC}"

update_config "ABSENT"
run_test "Тест 1: Файл отсутствует -> запись разрешена" "success"

update_config "EMPTY"
run_test "Тест 2: Файл пуст -> запись запрещена всем" "fail"

update_config "$TEST_UID"
run_test "Тест 3: Разрешенный пользователь -> запись разрешена" "success"

update_config "99999"
run_test "Тест 4: Запрещенный пользователь -> запись запрещена" "fail"

update_config "# Комментарий\n\n$TEST_UID\n# Конец"
run_test "Тест 5: Игнор комментариев и пустых строк -> успех" "success"

update_config "$TEST_UID \t Developer \t Dept"
run_test "Тест 6: Игнор лишних полей -> успех" "success"

update_config "11111"
run_test "Тест 7: Мгновенное применение (смена на запрет) -> запрет" "fail"

echo -e "\n${YELLOW}=== Очистка ===${NC}"
rm -f "$TEST_FILE" "$CONFIG_FILE"

echo -e "\n${YELLOW}=== Итоги ===${NC}"
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}Все $PASSED тестов успешно пройдены!${NC}"
else
    echo -e "${RED}Провалено тестов: $FAILED из $(($PASSED+$FAILED)).${NC}"
fi