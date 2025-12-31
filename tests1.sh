#!/bin/bash

# Цвета для вывода
GREEN='\033[1;32m'
RED='\033[1;31m'
BLUE='\033[1;34m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

TEST_DIR="test_artifacts"
VENV_DIR=".venv"

# 0. Подготовка окружения
echo -e "${BLUE}[INIT] Подготовка окружения...${NC}"

if [ -d "$TEST_DIR" ]; then rm -rf "$TEST_DIR"; fi
mkdir -p "$TEST_DIR"

# Проверка/Создание venv и установка networkx для генератора
if [ ! -d "$VENV_DIR" ]; then
    echo "Создание виртуального окружения Python..."
    python3 -m venv "$VENV_DIR"
    source "$VENV_DIR/bin/activate"
    pip install networkx > /dev/null 2>&1
else
    source "$VENV_DIR/bin/activate"
fi

# 1. Компиляция C++
echo -e "${BLUE}[INIT] Компиляция проекта...${NC}"
g++ -std=c++17 -pthread server.cpp graph.cpp protocol.cpp -o "$TEST_DIR/server"
if [ $? -ne 0 ]; then echo -e "${RED}[FAIL] Ошибка компиляции сервера${NC}"; exit 1; fi

g++ -std=c++17 -pthread client.cpp graph.cpp protocol.cpp -o "$TEST_DIR/client"
if [ $? -ne 0 ]; then echo -e "${RED}[FAIL] Ошибка компиляции клиента${NC}"; exit 1; fi

cd "$TEST_DIR" || exit 1

# 2. Создание Python скриптов (Генератор и Решатель)

cat << 'EOF' > gen_graph.py
import sys
import networkx as nx
import random

def generate_graph(filename, v_count, e_count):
    G = nx.gnm_random_graph(v_count, e_count, seed=42) # Seed для воспроизводимости структуры, но можно убрать
    # Веса
    for (u, v) in G.edges():
        G.edges[u, v]['weight'] = random.randint(1, 10)

    edges_list = list(G.edges(data=True))
    
    with open(filename, 'w') as f:
        f.write(f"{v_count} {len(edges_list)}\n")
        
        # Матрица инцидентности
        incidence_matrix = [[0] * len(edges_list) for _ in range(v_count)]
        weights = []
        for idx, (u, v, data) in enumerate(edges_list):
            incidence_matrix[u][idx] = 1
            incidence_matrix[v][idx] = 1
            weights.append(str(data['weight']))
            
        for row in incidence_matrix:
            f.write(" ".join(map(str, row)) + "\n")
        
        f.write(" ".join(weights) + "\n")

if __name__ == "__main__":
    generate_graph(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
EOF

cat << 'EOF' > solve_graph.py
import sys
import networkx as nx

def solve(filename, start_node, end_node):
    try:
        with open(filename, 'r') as f:
            lines = [l.strip() for l in f if l.strip()]
        
        header = lines[0].split()
        v_count = int(header[0])
        e_count = int(header[1])
        
        matrix_lines = lines[1:1+v_count]
        weight_line = lines[1+v_count]
        
        matrix = [list(map(int, l.split())) for l in matrix_lines]
        weights = list(map(int, weight_line.split()))
        
        G = nx.Graph()
        G.add_nodes_from(range(v_count))
        
        for e in range(e_count):
            nodes = [v for v in range(v_count) if matrix[v][e] == 1]
            if len(nodes) == 2:
                G.add_edge(nodes[0], nodes[1], weight=weights[e])
            # Петли или странные ребра игнорируем для простоты, или обрабатываем как (nodes[0], nodes[0])
            
        length = nx.shortest_path_length(G, source=start_node, target=end_node, weight='weight')
        print(length) # Выводим ТОЛЬКО число в stdout
    except nx.NetworkXNoPath:
        print("UNREACHABLE")
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    solve(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
EOF

# 3. Генерация тестовых данных
echo -e "${BLUE}[DATA] Генерация графов...${NC}"

# Малый граф (простой тест)
python3 gen_graph.py valid_small.txt 10 15
# Средний граф (нагрузка)
python3 gen_graph.py valid_medium.txt 100 200
# Большой граф (только для теста загрузки, pathfinding может быть долгим в expect)
# Используем разреженный граф, чтобы не раздувать матрицу до гигабайт
python3 gen_graph.py valid_large.txt 705 705 

# Невалидные файлы (ручное создание)
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

# 4. Скрипты Expect

# Скрипт проверки ПУТИ (Главный скрипт валидации)
cat << 'EOF' > test_path_verification.exp
set timeout 10
set protocol [lindex $argv 0]
set port [lindex $argv 1]
set filename [lindex $argv 2]
set u [lindex $argv 3]
set v [lindex $argv 4]
set expected_dist [lindex $argv 5]

log_user 0

spawn ./client 127.0.0.1 $protocol $port
expect "> "

# 1. Загрузка
send "load $filename\r"
expect {
    "Граф успешно загружен" { }
    timeout { puts "Фактический вывод: Timeout при загрузке"; exit 1 }
    "Ошибка" { puts "Фактический вывод: Ошибка при загрузке"; exit 1 }
}

expect "> "

# 2. Запрос пути
send "query $u $v\r"
expect {
    -re "Длина пути.*$expected_dist" {
        # Все отлично
        exit 0
    }
    -re "Длина пути.*" {
        set output $expect_out(0,string)
        puts "Фактический вывод: $output (Ожидалось: $expected_dist)"
        exit 1
    }
    timeout { puts "Фактический вывод: Timeout при запросе пути"; exit 1 }
}
EOF

# Скрипт проверки ОШИБОК (валидация ввода)
cat << 'EOF' > test_fail.exp
set timeout 5
set port [lindex $argv 0]
set filename [lindex $argv 1]
set expected_msg "Неверное количество вершин"

log_user 0

spawn ./client 127.0.0.1 tcp $port
expect "> "
send "load $filename\r"

expect {
    -re "Неверное количество.*" { exit 0 }
    timeout { puts "TIMEOUT"; exit 1 }
    "Граф успешно загружен" { puts "Граф загрузился, а не должен был"; exit 1 }
}
EOF

# Скрипт ручного ввода
cat << 'EOF' > test_manual.exp
set timeout 5
set port [lindex $argv 0]
log_user 0

spawn ./client 127.0.0.1 tcp $port
expect "> "
send "input\r"
expect "Введите данные:"
# Граф 6 вершин, кольцо
send "6 6\r"
send "1 0 0 0 0 1\r"
send "1 1 0 0 0 0\r"
send "0 1 1 0 0 0\r"
send "0 0 1 1 0 0\r"
send "0 0 0 1 1 0\r"
send "0 0 0 0 1 1\r"
send "1 1 1 1 1 1\r\r" 

expect "Граф успешно загружен"
expect "> "
send "query 0 3\r"
# В кольце из 6 вершин (веса 1) путь от 0 до 3 равен 3
expect {
    -re "Длина пути.*3" { exit 0 }
    timeout { puts "TIMEOUT manual check"; exit 1 }
}
EOF

# Скрипт UDP Timeout
cat << 'EOF' > test_udp_timeout.exp
set timeout 12
set port [lindex $argv 0]
log_user 0
spawn ./client 127.0.0.1 udp $port
expect "> "
send "load valid_small.txt\r"
expect {
     -re "Потеряна связь с сервером" { exit 0 }
     timeout { puts "TIMEOUT (UDP wait)"; exit 1 }
}
EOF

# --- ФУНКЦИЯ ЗАПУСКА ТЕСТА ---
run_path_test() {
    TEST_TITLE="$1"
    PORT="$2"
    PROTO="$3"
    FILE="$4"
    U="$5"
    V="$6"

    echo -e "${CYAN}TEST: $TEST_TITLE ($FILE) [$PROTO]${NC}"

    # 1. Считаем эталонное значение на Python
    EXPECTED_VAL=$(python3 solve_graph.py "$FILE" "$U" "$V")
    if [ $? -ne 0 ]; then
        echo -e "${RED}[ERROR] Python не смог посчитать путь. Проверьте установку networkx.${NC}"
        return
    fi

    # 2. Запускаем сервер
    ./server $PROTO $PORT > server.log 2>&1 &
    PID=$!
    sleep 0.5

    # 3. Запускаем тест клиента, передавая ему ожидаемое значение
    expect -f test_path_verification.exp "$PROTO" "$PORT" "$FILE" "$U" "$V" "$EXPECTED_VAL"
    RET=$?

    # 4. Убиваем сервер
    kill $PID 2>/dev/null
    wait $PID 2>/dev/null

    if [ $RET -eq 0 ]; then
        echo -e "Путь ($U -> $V) = $EXPECTED_VAL. Статус: ${GREEN}[PASS]${NC}"
    else
        echo -e "Ожидалось: $EXPECTED_VAL. Статус: ${RED}[FAIL]${NC}"
    fi
}

run_fail_test() {
    TEST_TITLE="$1"
    PORT="$2"
    FILE="$3"
    
    echo -e "${CYAN}TEST: $TEST_TITLE${NC}"
    ./server tcp $PORT > /dev/null 2>&1 &
    PID=$!
    sleep 0.5
    
    expect -f test_fail.exp "$PORT" "$FILE"
    RET=$?
    
    kill $PID 2>/dev/null
    wait $PID 2>/dev/null
    
    if [ $RET -eq 0 ]; then echo -e "Статус: ${GREEN}[PASS]${NC}"; else echo -e "Статус: ${RED}[FAIL]${NC}"; fi
}


# 1. Базовая проверка TCP (малый граф)
run_path_test "1. TCP Small Logic" 6001 "tcp" "valid_small.txt" 0 5

# 2. Базовая проверка UDP (малый граф)
run_path_test "2. UDP Small Logic" 6002 "udp" "valid_small.txt" 1 4

# 3. Средний граф (TCP) - проверка алгоритма на большем масштабе
run_path_test "3. TCP Medium Logic" 6003 "tcp" "valid_medium.txt" 0 50

# 4. Большой граф (Загрузка + простой путь)
# Проверяем соседние вершины, чтобы дейкстра не искал слишком долго, если реализация медленная
run_path_test "4. TCP Large (1000 nodes)" 6004 "tcp" "valid_large.txt" 0 1 

# 5. Ручной ввод и проверка пути
echo -e "${CYAN}TEST: 5. Manual Input & Logic${NC}"
./server tcp 6005 > /dev/null 2>&1 &
PID=$!
sleep 0.5
expect -f test_manual.exp 6005
if [ $? -eq 0 ]; then echo -e "Статус: ${GREEN}[PASS]${NC}"; else echo -e "Статус: ${RED}[FAIL]${NC}"; fi
kill $PID 2>/dev/null; wait $PID 2>/dev/null

# 6. Ошибки
run_fail_test "6. Validation Low (5 vertices)" 6006 "invalid_low_5.txt"
run_fail_test "7. Validation High (65536 vertices)" 6007 "invalid_too_big.txt"

# 8. UDP Timeout
echo -e "${CYAN}TEST: 8. UDP Timeout${NC}"
expect -f test_udp_timeout.exp 6008
if [ $? -eq 0 ]; then echo -e "Статус: ${GREEN}[PASS]${NC}"; else echo -e "Статус: ${RED}[FAIL]${NC}"; fi