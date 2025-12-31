#include "graph.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace graph {

namespace {

// Значение бесконечности для алгоритма кратчайшего пути (используется для недостижимых вершин).
constexpr uint32_t kInfinity = std::numeric_limits<uint32_t>::max() / 4; // делим на 4 для избежания переполнения при сложении в алгоритме Беллмана-Форда

// Внутренняя структура для представления ребра графа.
struct EdgeData {
    uint16_t u;      // Начальная вершина
    uint16_t v;      // Конечная вершина
    uint32_t weight; // Вес ребра
};

// Сборка списка рёбер из матрицы инцидентности: преобразует матрицу в список рёбер (u, v, weight).
// Для каждого столбца матрицы находит инцидентные вершины и создаёт соответствующее ребро.
// Поддерживает петли
// В случае ошибки записывает описание в параметр message и возвращает пустой список.
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

        if (endpoints.empty()) {
            message = "Найден столбец матрицы без инцидентных вершин.";
            edges.clear();
            return edges;
        }
        if (endpoints.size() > 2) {
            message = "Ребро соединяет более двух вершин.";
            edges.clear();
            return edges;
        }

        const uint32_t weight = definition.weights[e];
        if (weight == kInfinity) {
            message = "Вес ребра превышает допустимый диапазон.";
            edges.clear();
            return edges;
        }

        if (endpoints.size() == 1) {
            edges.push_back({endpoints[0], endpoints[0], weight});
        } else {
            edges.push_back({endpoints[0], endpoints[1], weight});
        }
    }
    return edges;
}

}  // namespace

// Валидация графа: проверяет соответствие графа всем требованиям.
// Проверяет: минимальное количество вершин (>= 6), минимальное количество рёбер (>= 6),
// соответствие размеров матрицы, корректность матрицы инцидентности, неотрицательность весов.
// Возвращает ValidationResult с результатом проверки.
ValidationResult validateGraph(const GraphDefinition& graph) {
    ValidationResult result;

    for (uint32_t weight : graph.weights) {
        if (weight > kInfinity) {
            result.message = "Вес ребра слишком велик.";
            return result;
        }
    }

    std::string message;
    for (uint16_t e = 0; e < graph.edgeCount; ++e) {
        uint16_t ones = 0;
        for (uint16_t v = 0; v < graph.vertexCount; ++v) {
            const int value = graph.incidence[v][e];
            if (value != 0 && value != 1) {
                message = "Матрица инцидентности должна содержать только 0 или 1.";
                result.message = message;
                return result;
            }
            if (value == 1) {
                ++ones;
            }
        }
        if (ones < 2) {
            message = "Каждое ребро должно быть инцидентно двум вершинам.";
            result.message = message;
            return result;
        }
        if (ones > 2) {
            message = "Ребро не может соединять более двух вершин.";
            result.message = message;
            return result;
        }
    }

    result.ok = true;
    return result;
}

// Алгоритм Беллмана-Форда для поиска кратчайшего пути в неориентированном графе.
// Выполняет V-1 итераций релаксации всех рёбер для нахождения кратчайших расстояний от source.
// Для неориентированного графа релаксация выполняется в обе стороны каждого ребра.
// После вычисления расстояний восстанавливает путь по массиву предшественников.
// Возвращает PathComputation с информацией о пути от source до target.
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

    ValidationResult validation = validateGraph(graph);
    if (!validation.ok) {
        result.error = validation.message;
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
    parent[source] = -1;  // Исходная вершина не имеет предшественника

    for (uint16_t iter = 0; iter < n - 1; ++iter) {
        bool updated = false;
        for (const auto& edge : edges) {
            const uint16_t u = edge.u;
            const uint16_t v = edge.v;
            const uint32_t w = edge.weight;

            // Релаксация ребра u -> v
            if (dist[u] != kInfinity && dist[u] + w < dist[v]) {
                dist[v] = dist[u] + w;
                parent[v] = static_cast<int16_t>(u);
                updated = true;
            }
            // Релаксация ребра v -> u (для неориентированного графа)
            if (dist[v] != kInfinity && dist[v] + w < dist[u]) {
                dist[u] = dist[v] + w;
                // Не обновляем parent[source], так как это исходная вершина
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

// // Преобразование графа в список рёбер: создаёт список рёбер из матрицы инцидентности.
// // Также выполняет валидацию графа и записывает результат в параметр status.
// // Возвращает пустой список, если валидация не пройдена или произошла ошибка при сборке рёбер.
// std::vector<Edge> buildEdgeList(const GraphDefinition& graphDef, ValidationResult& status) {
//     status = validateGraph(graphDef);
//     if (!status.ok) {
//         return {};
//     }
//     std::string message;
//     std::vector<EdgeData> edges = collectEdges(graphDef, message);
//     if (!message.empty()) {
//         status.ok = false;
//         status.message = message;
//         return {};
//     }
//     std::vector<Edge> result;
//     result.reserve(edges.size());
//     for (const auto& e : edges) {
//         result.emplace_back(e.u, e.v, e.weight);
//     }
//     return result;
// }

}  // namespace graph


