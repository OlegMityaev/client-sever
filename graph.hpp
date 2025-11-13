// Структуры для хранения графа, результатов валидации и поиска кратчайшего пути

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace graph {

// Структура, описывающая граф: количество вершин и рёбер, матрица инцидентности и веса рёбер.
struct GraphDefinition {
    uint16_t vertexCount = 0;                    // Количество вершин в графе
    uint16_t edgeCount = 0;                      // Количество рёбер в графе
    std::vector<std::vector<int>> incidence;    // Матрица инцидентности (вершины x рёбра, значения 0 или 1)
    std::vector<uint32_t> weights;               // Список весов рёбер (индекс соответствует номеру ребра)
};

// Результат валидации графа: содержит флаг успешности и сообщение об ошибке (если есть).
struct ValidationResult {
    bool ok = false;        // true, если граф корректен
    std::string message;    // Сообщение об ошибке
};

// Результат вычисления кратчайшего пути: содержит информацию о достижимости, длине и маршруте.
struct PathComputation {
    bool reachable = false;              // true, если путь между вершинами существует
    uint32_t distance = 0;              // Длина кратчайшего пути (или INF, если путь не найден)
    std::vector<uint16_t> path;          // Последовательность вершин кратчайшего пути
    std::string error;                   // Сообщение об ошибке (если вычисление не удалось)
};

// Валидация графа: проверяет корректность структуры графа согласно требованиям.
// Проверяет: количество вершин (>= 6), количество рёбер (>= 6), корректность матрицы инцидентности,
// неотрицательность весов. Возвращает ValidationResult с результатом проверки.
ValidationResult validateGraph(const GraphDefinition& graph);

// Поиск кратчайшего пути алгоритмом Беллмана-Форда в неориентированном графе.
// Алгоритм выполняет V-1 итераций релаксации всех рёбер для нахождения кратчайших расстояний.
// Возвращает PathComputation с информацией о пути от source до target.
PathComputation bellmanFord(const GraphDefinition& graph, uint16_t source, uint16_t target);

// Тип для представления ребра: (начальная вершина, конечная вершина, вес).
using Edge = std::tuple<uint16_t, uint16_t, uint32_t>;

// Преобразование графа в список рёбер для удобной обработки.
// Также выполняет валидацию графа и записывает результат в параметр status.
std::vector<Edge> buildEdgeList(const GraphDefinition& graph, ValidationResult& status);

}  // namespace graph


