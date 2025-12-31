#!/bin/bash

GREEN='\033[1;32m'
RED='\033[1;31m'
BLUE='\033[1;34m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

TEST_DIR="test_artifacts"
VENV_DIR=".venv"

if [ -d "$TEST_DIR" ]; then rm -rf "$TEST_DIR"; fi
mkdir -p "$TEST_DIR"

# Проверка/Создание venv
if [ ! -d "$VENV_DIR" ]; then
    echo -e "${YELLOW}[INIT] Создание виртуального окружения Python...${NC}"
    python3 -m venv "$VENV_DIR"
    source "$VENV_DIR/bin/activate"
    pip install networkx > /dev/null 2>&1
else
    source "$VENV_DIR/bin/activate"
fi

echo -e "${YELLOW}[INIT] Компиляция C++ проекта...${NC}"
g++ -std=c++17 -pthread server.cpp graph.cpp protocol.cpp -o "$TEST_DIR/server"
if [ $? -ne 0 ]; then echo -e "${RED}[FAIL] Ошибка компиляции сервера${NC}"; exit 1; fi

g++ -std=c++17 -pthread client.cpp graph.cpp protocol.cpp -o "$TEST_DIR/client"
if [ $? -ne 0 ]; then echo -e "${RED}[FAIL] Ошибка компиляции клиента${NC}"; exit 1; fi

cd "$TEST_DIR" || exit 1

# Генератор графов
cat << 'EOF' > gen_graph.py
import sys
import networkx as nx
import random

def generate_graph(filename, v_count, e_count, disconnected=False):
    # Если нужен несвязный граф
    if disconnected:
        v1 = v_count // 2
        v2 = v_count - v1
        e1 = e_count // 2
        e2 = e_count - e1
        G1 = nx.gnm_random_graph(v1, e1, seed=42)
        G2 = nx.gnm_random_graph(v2, e2, seed=43)
        mapping = {i: i + v1 for i in range(v2)}
        G2 = nx.relabel_nodes(G2, mapping)
        G = nx.compose(G1, G2)
    else:
        # Пытаемся создать связный граф
        attempt = 0
        while attempt < 100:
            try:
                G = nx.gnm_random_graph(v_count, e_count, seed=42+attempt)
                if v_count > 1 and e_count >= v_count - 1:
                    if nx.is_connected(G):
                        break
                else:
                    break
            except:
                pass
            attempt += 1
    # G = nx.gnm_random_graph(v_count, e_count, seed=42)
    for (u, v) in G.edges():
        G.edges[u, v]['weight'] = random.randint(1, 100000)

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
    is_disc = False
    if len(sys.argv) > 4 and sys.argv[4] == "disconnected":
        is_disc = True
    generate_graph(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), is_disc)
EOF

# Решатель
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
            
        try:
            length = nx.shortest_path_length(G, source=start_node, target=end_node, weight='weight')
            print(f"Длина пути: {length}")
        except nx.NetworkXNoPath:
            print("Ошибка сервера: Путь между вершинами не найден.") # Это ожидаемая строка для несвязных
            
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    solve(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
EOF

echo -e "${YELLOW}[DATA] Генерация тестовых наборов...${NC}"


python3 gen_graph.py valid_small.txt 10 15
python3 gen_graph.py valid_medium.txt 100 200
python3 gen_graph.py valid_max_limit.txt 705 705
python3 gen_graph.py disconnected.txt 20 10 disconnected

# Невалидные файлы
# 1. Слишком мало вершин (<6)
cat << EOF > invalid_low_5.txt
5 5
1 0 0 0 1
1 1 0 0 0
0 1 1 0 0
0 0 1 1 0
0 0 0 1 1
1 1 1 1 1
EOF

python3 gen_graph.py invalid_limit_exceeded.txt 706 706

python3 gen_graph.py valid_huge_sparse.txt 65535 7

cat << EOF > invalid_overflow.txt
65536 7
$(for i in {1..65536}; do echo "0 0 0 0 0 0 0"; done)
1 1 1 1 1 1 1
EOF

# expect

cat << 'EOF' > run_test_logic.exp
set timeout 10
set protocol [lindex $argv 0]
set port [lindex $argv 1]
set filename [lindex $argv 2]
set cmd_query [lindex $argv 3]
set expected_out [lindex $argv 4]

log_user 0

proc print_res {input expected actual status} {
    puts "   INPUT    : $input"
    puts "   EXPECTED : $expected"
    puts "   ACTUAL   : $actual"
    if {$status == "PASS"} {
        puts "   RESULT   : \033\[1;32m\[PASS\]\033\[0m"
    } else {
        puts "   RESULT   : \033\[1;31m\[FAIL\]\033\[0m"
    }
}

spawn ./client 127.0.0.1 $protocol $port
expect "> "

# Этап 1: Загрузка
send "load $filename\r"
expect {
    "Граф успешно загружен" { }
    timeout { 
        print_res "load $filename" "Граф успешно загружен" "TIMEOUT" "FAIL"
        exit 1 
    }
    -re "Ошибка.*|Неверное.*" {
        print_res "load $filename" "Граф успешно загружен" "$expect_out(0,string)" "FAIL"
        exit 1
    }
}

expect "> "

# Этап 2: Запрос
send "$cmd_query\r"
expect {
    -re "$expected_out" {
        print_res "$cmd_query" "$expected_out" "$expect_out(0,string)" "PASS"
        exit 0
    }
    -re "Длина пути.*|Путь не существует.*|Не удалось.*" {
        print_res "$cmd_query" "$expected_out" "$expect_out(0,string)" "FAIL"
        exit 1
    }
    timeout {
        print_res "$cmd_query" "$expected_out" "TIMEOUT" "FAIL"
        exit 1
    }
}
EOF

# Скрипт для проверки ошибок (валидации)
cat << 'EOF' > run_test_validation.exp
set timeout 5
set port [lindex $argv 0]
set filename [lindex $argv 1]
set expected_err "Неверное количество вершин"

log_user 0

proc print_res {input expected actual status} {
    puts "   INPUT    : $input"
    puts "   EXPECTED : Error ($expected)"
    puts "   ACTUAL   : $actual"
    if {$status == "PASS"} {
        puts "   RESULT   : \033\[1;32m\[PASS\]\033\[0m"
    } else {
        puts "   RESULT   : \033\[1;31m\[FAIL\]\033\[0m"
    }
}

spawn ./client 127.0.0.1 tcp $port
expect "> "
send "load $filename\r"

expect {
    -re "Неверное количество.*|Ошибка.*" {
        print_res "load $filename" "$expected_err" "$expect_out(0,string)" "PASS"
        exit 0
    }
    "Граф успешно загружен" {
        print_res "load $filename" "$expected_err" "Граф успешно загружен" "FAIL"
        exit 1
    }
    timeout {
        print_res "load $filename" "$expected_err" "TIMEOUT" "FAIL"
        exit 1
    }
}
EOF

# Скрипт для проверки таймаута UDP
cat << 'EOF' > run_test_udp_timeout.exp
set timeout 12
set port [lindex $argv 0]

log_user 0

proc print_res {input expected actual status} {
    puts "   INPUT    : $input"
    puts "   EXPECTED : $expected"
    puts "   ACTUAL   : $actual"
    if {$status == "PASS"} {
        puts "   RESULT   : \033\[1;32m\[PASS\]\033\[0m"
    } else {
        puts "   RESULT   : \033\[1;31m\[FAIL\]\033\[0m"
    }
}

spawn ./client 127.0.0.1 udp $port
expect "> "
send "load valid_small.txt\r"
expect {
     -re "Потеряна связь с сервером" { 
        print_res "load valid_small.txt (Server OFF)" "Потеряна связь с сервером" "Потеряна связь с сервером" "PASS"
        exit 0 
     }
     timeout { 
        print_res "load valid_small.txt (Server OFF)" "Потеряна связь..." "TIMEOUT (клиент завис)" "FAIL"
        exit 1 
     }
}
EOF

run_logic_test() {
    TEST_NAME="$1"
    PORT="$2"
    PROTO="$3"
    FILE="$4"
    U="$5"
    V="$6"

    echo -e "${CYAN}TEST: $TEST_NAME${NC}"

    # 1. Получаем эталон
    EXPECTED_VAL=$(python3 solve_graph.py "$FILE" "$U" "$V")
    
    # 2. Старт сервера
    ./server $PROTO $PORT > /dev/null 2>&1 &
    PID=$!
    sleep 0.5

    # 3. Запуск теста
    expect -f run_test_logic.exp "$PROTO" "$PORT" "$FILE" "query $U $V" "$EXPECTED_VAL"
    RET=$?

    # 4. Стоп сервер
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    
    if [ $RET -eq 0 ]; then return 0; else return 1; fi
}

run_validation_test() {
    TEST_NAME="$1"
    PORT="$2"
    FILE="$3"

    echo -e "${CYAN}TEST: $TEST_NAME${NC}"
    ./server tcp $PORT > /dev/null 2>&1 &
    PID=$!
    sleep 0.5
    expect -f run_test_validation.exp "$PORT" "$FILE"
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
}


run_logic_test "1. TCP: Малый граф (10 вершин)" 6001 "tcp" "valid_small.txt" 0 5
run_logic_test "2. UDP: Малый граф (10 вершин)" 6002 "udp" "valid_small.txt" 1 4
run_logic_test "3. TCP: Средний граф (100 вершин)" 6003 "tcp" "valid_medium.txt" 0 50
run_logic_test "4. TCP: Макс. граф (705 вершин, 705 ребер)" 6004 "tcp" "valid_max_limit.txt" 0 704
run_logic_test "5. TCP: Несвязный граф (пути нет)" 6005 "tcp" "disconnected.txt" 0 15
run_validation_test "6. Ошибка: 5 вершин (< min 6)" 6006 "invalid_low_5.txt"
run_validation_test "7. Ошибка: 706 вершин и 706 рёбер (> max 705)" 6007 "invalid_limit_exceeded.txt"
echo -e "${CYAN}TEST: 8. UDP Reliability (Нет сервера - таймаут)${NC}"
expect -f run_test_udp_timeout.exp 6008
echo -e "${CYAN}TEST: 9. Concurrency (3 клиента одновременно)${NC}"
./server tcp 7000 > /dev/null 2>&1 &
SERVER_PID=$!
sleep 1

# Запуск клиентов в фоне
(
    expect -c "
    log_user 0
    spawn ./client 127.0.0.1 tcp 7000
    expect \"> \"
    send \"load valid_small.txt\r\"
    expect \"Граф успешно загружен\"
    send \"query 0 5\r\"
    expect \"Длина пути\"
    exit 0
    " 
) & PID1=$!

(
    expect -c "
    log_user 0
    spawn ./client 127.0.0.1 tcp 7000
    expect \"> \"
    send \"load valid_medium.txt\r\"
    expect \"Граф успешно загружен\"
    send \"query 0 50\r\"
    expect \"Длина пути\"
    exit 0
    " 
) & PID2=$!

(
    expect -c "
    log_user 0
    spawn ./client 127.0.0.1 tcp 7000
    expect \"> \"
    send \"load valid_max_limit.txt\r\"
    expect \"Граф успешно загружен\"
    send \"query 0 1\r\"
    expect \"Длина пути\"
    exit 0
    " 
) & PID3=$!

wait $PID1
R1=$?
wait $PID2
R2=$?
wait $PID3
R3=$?

kill $SERVER_PID 2>/dev/null
echo "   Клиент 1 (Small) Status: $R1"
echo "   Клиент 2 (Medium) Status: $R2"
echo "   Клиент 3 (Max) Status: $R3"

if [ $R1 -eq 0 ] && [ $R2 -eq 0 ] && [ $R3 -eq 0 ]; then
    echo -e "   RESULT   : ${GREEN}[PASS] Все клиенты отработали${NC}"
else
    echo -e "   RESULT   : ${RED}[FAIL] Ошибка параллельной работы${NC}"
fi

run_logic_test "10. TCP: граница вершин (65535 вершин, 7 рёбер)" 6004 "tcp" "valid_huge_sparse.txt" 0 1

run_validation_test "11. Ошибка: 65536 вершин" 6007 "invalid_overflow.txt"
