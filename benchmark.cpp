#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

constexpr uint32_t kInfinity = std::numeric_limits<uint32_t>::max() / 4;

struct GraphDefinition {
    uint16_t vertexCount = 0;
    uint16_t edgeCount = 0;
    std::vector<std::vector<int>> incidence;
    std::vector<uint32_t> weights;
};

struct PathComputation {
    bool reachable = false;
    uint32_t distance = 0;
    std::vector<uint16_t> path;
    std::string error;
};

struct EdgeData {
    uint16_t u;
    uint16_t v;
    uint32_t weight;
};

std::vector<EdgeData> collectEdges(const GraphDefinition& definition, std::string& message) {
    std::vector<EdgeData> edges;
    edges.reserve(definition.edgeCount);

    for (uint16_t e = 0; e < definition.edgeCount; ++e) {
        std::vector<uint16_t> endpoints;
        endpoints.reserve(2);
        for (uint16_t v = 0; v < definition.vertexCount; ++v) {
            if (definition.incidence[v][e] == 1) {
                endpoints.push_back(v);
            }
        }

        if (endpoints.empty() || endpoints.size() > 2) {
            message = "Invalid edge";
            edges.clear();
            return edges;
        }

        const uint32_t weight = definition.weights[e];
        if (endpoints.size() == 1) {
            edges.push_back({endpoints[0], endpoints[0], weight});
        } else {
            edges.push_back({endpoints[0], endpoints[1], weight});
        }
    }
    return edges;
}

PathComputation bellmanFord(const GraphDefinition& graph, uint16_t source, uint16_t target) {
    PathComputation result;

    if (graph.vertexCount == 0) {
        result.error = "Граф не инициализирован.";
        return result;
    }
    if (source >= graph.vertexCount || target >= graph.vertexCount) {
        result.error = "Вершины выходят за границы графа.";
        return result;
    }

    std::string edgeError;
    std::vector<EdgeData> edges = collectEdges(graph, edgeError);
    if (!edgeError.empty()) {
        result.error = edgeError;
        return result;
    }

    const uint16_t n = graph.vertexCount;
    std::vector<uint32_t> dist(n, kInfinity);
    std::vector<int16_t> parent(n, -1);

    dist[source] = 0;
    parent[source] = -1;

    for (uint16_t iter = 0; iter < n - 1; ++iter) {
        bool updated = false;
        for (const auto& edge : edges) {
            const uint16_t u = edge.u;
            const uint16_t v = edge.v;
            const uint32_t w = edge.weight;

            if (dist[u] != kInfinity && dist[u] + w < dist[v]) {
                dist[v] = dist[u] + w;
                parent[v] = static_cast<int16_t>(u);
                updated = true;
            }
            if (dist[v] != kInfinity && dist[v] + w < dist[u]) {
                dist[u] = dist[v] + w;
                if (u != source) {
                    parent[u] = static_cast<int16_t>(v);
                }
                updated = true;
            }
        }
        if (!updated) {
            break;
        }
    }

    if (dist[target] == kInfinity) {
        result.reachable = false;
        result.distance = kInfinity;
        result.path.clear();
        result.error = "Путь между вершинами не найден.";
        return result;
    }

    std::vector<uint16_t> path;
    for (int16_t v = static_cast<int16_t>(target); v != -1; v = parent[v]) {
        path.push_back(static_cast<uint16_t>(v));
        if (v == static_cast<int16_t>(source)) {
            break;
        }
    }
    std::reverse(path.begin(), path.end());

    if (path.empty() || path.front() != source) {
        result.error = "Не удалось восстановить путь.";
        return result;
    }

    result.reachable = true;
    result.distance = dist[target];
    result.path = std::move(path);
    return result;
}

GraphDefinition generateCompleteGraph(uint16_t n) {
    GraphDefinition graph;
    graph.vertexCount = n;
    graph.edgeCount = n * (n - 1) / 2;
    
    graph.incidence.assign(n, std::vector<int>(graph.edgeCount, 0));
    graph.weights.resize(graph.edgeCount);
    
    uint16_t edgeIdx = 0;
    for (uint16_t i = 0; i < n; ++i) {
        for (uint16_t j = i + 1; j < n; ++j) {
            graph.incidence[i][edgeIdx] = 1;
            graph.incidence[j][edgeIdx] = 1;
            graph.weights[edgeIdx] = 1;
            ++edgeIdx;
        }
    }
    
    return graph;
}

int main() {
    uint16_t maxVertices = 0;
    uint32_t maxEdges = 0;
    
    for (uint16_t n = 6; n <= 1000; ++n) {
        GraphDefinition graph = generateCompleteGraph(n);
        uint32_t edges = n * (n - 1) / 2;
        
        auto start = std::chrono::high_resolution_clock::now();
        PathComputation result = bellmanFord(graph, 0, n - 1);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        double seconds = duration.count() / 1000.0;
        
        if (seconds < 1.0) {
            maxVertices = n;
            maxEdges = edges;
            std::cout << "n=" << n << " edges=" << edges << " time=" << seconds << "s" << std::endl;
        } else {
            std::cout << "n=" << n << " edges=" << edges << " time=" << seconds << "s (превышен лимит)" << std::endl;
            break;
        }
    }
    
    std::cout << "\nМаксимальный граф: " << maxVertices << " вершин, " << maxEdges << " рёбер" << std::endl;
    
    return 0;
}

