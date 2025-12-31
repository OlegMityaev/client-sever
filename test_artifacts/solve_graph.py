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
