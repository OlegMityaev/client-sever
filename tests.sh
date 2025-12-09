#!/bin/bash


GREEN='\033[1;32m'
RED='\033[1;31m'
BLUE='\033[1;34m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

TEST_DIR="test_artifacts"

# Проверка компиляции
if [ -d "$TEST_DIR" ]; then rm -rf "$TEST_DIR"; fi
mkdir -p "$TEST_DIR"

echo -e "${BLUE}[INIT] Компиляция проекта...${NC}"
g++ -std=c++17 -pthread server.cpp graph.cpp protocol.cpp -o "$TEST_DIR/server"
if [ $? -ne 0 ]; then echo -e "${RED}[FAIL] Ошибка компиляции сервера${NC}"; exit 1; fi

g++ -std=c++17 -pthread client.cpp graph.cpp protocol.cpp -o "$TEST_DIR/client"
if [ $? -ne 0 ]; then echo -e "${RED}[FAIL] Ошибка компиляции клиента${NC}"; exit 1; fi

cd "$TEST_DIR" || exit 1

# код генерации данных
cat << 'EOF' > gen_graph.py
import sys

def generate_cycle_graph(filename, v_count, e_count):
    try:
        with open(filename, 'w') as f:
            f.write(f"{v_count} {e_count}\n")
            # Генерируем "кольцо" или цепочку
            for row in range(v_count):
                row_vals = []
                for e in range(e_count):
                    val = "0"
                    v_start = e
                    v_end = (e + 1) % v_count
                    if row == v_start or row == v_end:
                        val = "1"
                    row_vals.append(val)
                f.write(" ".join(row_vals) + "\n")
            weights = ["1"] * e_count
            f.write(" ".join(weights) + "\n")
    except Exception as e:
        sys.exit(1)

if __name__ == "__main__":
    generate_cycle_graph(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
EOF

# Генерация тестовых данных
python3 gen_graph.py valid_min_6.txt 6 6
python3 gen_graph.py valid_middle_30k.txt 30000 10
python3 gen_graph.py valid_max_huge.txt 65535 7

cat << EOF > invalid_low_5.txt
5 5
1 0 0 0 1
1 1 0 0 0
0 1 1 0 0
0 0 1 1 0
0 0 0 1 1
1 1 1 1 1
EOF

cat << EOF > invalid_too_big.txt
65536 6
0 0 0 0 0 0
EOF


# Проверка работы
cat << 'EOF' > test_success.exp
set timeout 10
set protocol [lindex $argv 0]
set port [lindex $argv 1]
set filename [lindex $argv 2]
set cmd_input "load $filename"
set expected_msg "Граф успешно загружен"

log_user 0 ;# Скрываем лишний шум

spawn ./client 127.0.0.1 $protocol $port
expect "> "
send "$cmd_input\r"

puts "Ввод:             $cmd_input"
puts "Ожидаемый вывод:  $expected_msg"

expect {
    -re "Граф успешно загружен" {
        # Получаем строку из буфера для вывода
        puts "Фактический вывод: Граф успешно загружен"
        # Проверяем query для надежности
        expect "> "
        send "query 0 1\r"
        expect {
            "Длина пути" { exit 0 } # PASS
            timeout { puts "Фактический вывод (Query): Timeout"; exit 1 }
        }
    }
    timeout {
        set output $expect_out(buffer)
        # Очищаем вывод от мусора (символы новой строки) для красоты
        regsub -all {\n} $output " " output
        puts "Фактический вывод: (TIMEOUT) $output"
        exit 1
    }
    -re "Ошибка.*" {
        set output $expect_out(buffer)
        puts "Фактический вывод: $output"
        exit 1
    }
}
EOF

# 5 вершин (меньше необходимого)
cat << 'EOF' > test_fail.exp
set timeout 5
set port [lindex $argv 0]
set filename [lindex $argv 1]
set cmd_input "load $filename"
set expected_msg "Ошибка валидации (Неверное кол-во...)"

log_user 0

spawn ./client 127.0.0.1 tcp $port
expect "> "
send "$cmd_input\r"

puts "Ввод:             $cmd_input"
puts "Ожидаемый вывод:  $expected_msg"

expect {
    -re "Неверное количество.*" {
        # Выцепляем конкретное сообщение об ошибке
        set full_buff $expect_out(buffer)
        # Берем последнюю строку или часть с ошибкой
        if {[regexp {Неверное количество [^\r\n]*} $full_buff match]} {
             puts "Фактический вывод: $match"
        } else {
             puts "Фактический вывод: $full_buff"
        }
        exit 0
    }
    -re "Граф успешно загружен" {
        puts "Фактический вывод: Граф успешно загружен (А должна быть ошибка!)"
        exit 1
    }
    timeout {
        puts "Фактический вывод: (TIMEOUT)"
        exit 1
    }
}
EOF

# Тест ручного ввода
cat << 'EOF' > test_manual.exp
set timeout 5
set port [lindex $argv 0]
log_user 0

spawn ./client 127.0.0.1 tcp $port
expect "> "
puts "Ввод:             input (матрица 6x6 вручную)"
puts "Ожидаемый вывод:  Граф успешно загружен"

send "input\r"
expect "Введите данные:"
send "6 6\r"
send "1 0 0 0 0 1\r"
send "1 1 0 0 0 0\r"
send "0 1 1 0 0 0\r"
send "0 0 1 1 0 0\r"
send "0 0 0 1 1 0\r"
send "0 0 0 0 1 1\r"
send "1 1 1 1 1 1\r\r"

expect {
    "Граф успешно загружен" {
        puts "Фактический вывод: Граф успешно загружен"
        exit 0
    }
    timeout {
        puts "Фактический вывод: (TIMEOUT) Клиент не ответил"
        exit 1
    }
}
EOF

# Сценарий UDP TIMEOUT
cat << 'EOF' > test_udp_timeout.exp
set timeout 12
set port [lindex $argv 0]

log_user 0

spawn ./client 127.0.0.1 udp $port
expect "> "

puts "Ввод:             load valid_min_6.txt (Сервер выключен)"
puts "Ожидаемый вывод:  (Нет ответа, попытка 1)\n                  (Нет ответа, попытка 2)\n                  (Нет ответа, попытка 3)\n                  Потеряна связь с сервером."

send "load valid_min_6.txt\r"
expect {
    -re {\(Нет ответа, попытка 1\)[\s\S]*\(Нет ответа, попытка 2\)[\s\S]*\(Нет ответа, попытка 3\)[\s\S]*Потеряна связь с сервером} {
        set output $expect_out(buffer)
        regsub -all {\r} $output "" output
        regsub -all {^\s+|\s+$} $output "" output
        regsub -all {\n} $output "\n                  " output
        puts "Фактический вывод: $output"
        exit 0
    }
    timeout {
        set output "(TIMEOUT) Сообщение не появилось или появилось не полностью."
        if {[info exists expect_out(buffer)]} {
            set received $expect_out(buffer)
            regsub -all {\r} $received "" received
            regsub -all {^\s+|\s+$} $received "" received
            if {$received ne ""} {
                regsub -all {\n} $received "\n                  " received
                append output "\n                  (Получено: $received)"
            }
        }
        puts "Фактический вывод: $output"
        exit 1
    }
}
EOF

# Функция запуска теста
run_test_block() {
    TEST_TITLE="$1"
    PORT="$2"
    CMD="$3"
    PROTO="$4"

    echo -e "${CYAN}TEST: $TEST_TITLE${NC}"

    # Запуск сервера
    PID=""
    if [ "$PROTO" != "NONE" ]; then
        ./server $PROTO $PORT > /dev/null 2>&1 &
        PID=$!
        sleep 0.5
    fi

    # Выполнение expect (он сам напечатает Ввод/Ожидание/Факт)
    eval $CMD
    RET=$?

    # Убийство сервера
    if [ "$PID" != "" ]; then
        kill $PID 2>/dev/null
        wait $PID 2>/dev/null
    fi

    # Вердикт
    if [ $RET -eq 0 ]; then
        echo -e "Статус:           ${GREEN}[PASS]${NC}"
    else
        echo -e "Статус:           ${RED}[FAIL]${NC}"
    fi
}


# 1. TCP Min
run_test_block "1. TCP: Валидный граф (6 вершин)" 5001 \
    "expect -f test_success.exp tcp 5001 valid_min_6.txt" "tcp"

# 2. UDP Min
run_test_block "2. UDP: Валидный граф (6 вершин)" 5002 \
    "expect -f test_success.exp udp 5002 valid_min_6.txt" "udp"

# 3. Middle Graph
run_test_block "3. TCP: Средний граф (30000 вершин)" 5003 \
    "expect -f test_success.exp tcp 5003 valid_middle_30k.txt" "tcp"

# 4. Max Graph (65535)
run_test_block "4. TCP: Максимальный граф (65535 вершин)" 5004 \
    "expect -f test_success.exp tcp 5004 valid_max_huge.txt" "tcp"

# 5. Ручной ввод
run_test_block "5. Ручной ввод через консоль" 5005 \
    "expect -f test_manual.exp 5005" "tcp"

# 6. Ошибка (Min - 1)
run_test_block "6. Валидация: 5 вершин (Нижняя граница)" 5006 \
    "expect -f test_fail.exp 5006 invalid_low_5.txt" "tcp"

# 7. Ошибка (Max + 1)
run_test_block "7. Валидация: 65536 вершин (Верхняя граница)" 5007 \
    "expect -f test_fail.exp 5007 invalid_too_big.txt" "tcp"

# 8. UDP Timeout
run_test_block "8. UDP: Проверка таймаута (Сервер недоступен)" 5008 \
    "expect -f test_udp_timeout.exp 5008" "NONE"