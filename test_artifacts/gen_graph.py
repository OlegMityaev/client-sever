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
