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
