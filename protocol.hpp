// protocol.hpp
// Общие структуры данных и функции сериализации для обмена между клиентом и сервером.
// Протокол использует бинарный формат с фиксированным заголовком для оптимальной передачи данных.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace netproto {

// Размер заголовка сетевого сообщения в байтах (12 байт: command + status + requestId + payloadSize + reserved).
constexpr std::size_t kHeaderSize = 12;

// Коды команд, используемые в протоколе для идентификации типа запроса/ответа.
enum class Command : uint8_t {
    Help = 1,        // Запрос справки по командам
    UploadGraph = 2, // Загрузка графа на сервер
    PathQuery = 3,   // Запрос кратчайшего пути между вершинами
    PathResult = 4,  // Ответ с результатом поиска пути
    Error = 5,       // Сообщение об ошибке
    Ack = 6,         // Подтверждение получения (для UDP)
    Exit = 7         // Завершение соединения
};

// Статусы выполнения команды, указывающие на результат обработки запроса.
enum class Status : uint8_t {
    Ok = 0,              // Команда выполнена успешно
    InvalidRequest = 1,   // Некорректный запрос
    InternalError = 2,    // Внутренняя ошибка сервера
    NotReady = 3          // Сервер не готов (например, граф не загружен)
};

// Заголовок сообщения. Все числовые поля передаются в сетевом порядке (big endian).
// Используется для всех сообщений между клиентом и сервером.
struct MessageHeader {
    Command command;      // Тип команды
    Status status;        // Статус выполнения
    uint16_t requestId;   // Идентификатор запроса (для UDP, чтобы связать запрос и ответ)
    uint32_t payloadSize; // Размер полезной нагрузки в байтах
    uint32_t reserved;    // Зарезервированное поле для будущего использования
};

// Полезная нагрузка команды UploadGraph: содержит описание графа в компактном формате.
struct UploadGraphPayload {
    uint16_t vertexCount;              // Количество вершин в графе
    uint16_t edgeCount;                // Количество рёбер в графе
    std::vector<uint8_t> incidenceBits; // Матрица инцидентности в битовом формате (упакована)
    std::vector<uint32_t> weights;     // Список весов рёбер
};

// Полезная нагрузка команды PathQuery: запрос пути между двумя вершинами.
struct PathQueryPayload {
    uint16_t source; // Начальная вершина (нумерация с 0)
    uint16_t target; // Конечная вершина (нумерация с 0)
};

// Полезная нагрузка ответа PathResult: результат поиска кратчайшего пути.
struct PathResultPayload {
    uint32_t distance;              // Длина найденного пути
    std::vector<uint16_t> path;     // Последовательность вершин пути
};

// Сериализация заголовка: преобразует структуру MessageHeader в массив байтов для передачи по сети.
std::vector<uint8_t> serializeHeader(const MessageHeader& header);

// Десериализация заголовка: восстанавливает структуру MessageHeader из массива байтов.
// Возвращает false, если данные некорректны.
bool deserializeHeader(const std::vector<uint8_t>& buffer, MessageHeader& header);

// Сериализация полезной нагрузки UploadGraph: упаковывает граф в бинарный формат.
std::vector<uint8_t> serializeUploadGraph(const UploadGraphPayload& payload);

// Десериализация полезной нагрузки UploadGraph: восстанавливает граф из бинарного формата.
// В случае ошибки записывает описание в параметр error.
bool deserializeUploadGraph(const std::vector<uint8_t>& buffer, UploadGraphPayload& payload, std::string& error);

// Сериализация полезной нагрузки PathQuery: упаковывает запрос пути в бинарный формат.
std::vector<uint8_t> serializePathQuery(const PathQueryPayload& payload);

// Десериализация полезной нагрузки PathQuery: восстанавливает запрос пути из бинарного формата.
bool deserializePathQuery(const std::vector<uint8_t>& buffer, PathQueryPayload& payload);

// Сериализация полезной нагрузки PathResult: упаковывает результат поиска пути в бинарный формат.
std::vector<uint8_t> serializePathResult(const PathResultPayload& payload);

// Десериализация полезной нагрузки PathResult: восстанавливает результат из бинарного формата.
// В случае ошибки записывает описание в параметр error.
bool deserializePathResult(const std::vector<uint8_t>& buffer, PathResultPayload& payload, std::string& error);

// Утилита для упаковки строки в полезную нагрузку (используется для ошибок и help).
// Формат: 2 байта (длина строки) + байты строки.
std::vector<uint8_t> serializeString(const std::string& text);

// Десериализация строки из полезной нагрузки.
bool deserializeString(const std::vector<uint8_t>& buffer, std::string& text);

// Раскодировка матрицы инцидентности: преобразует битовую последовательность в двумерную матрицу.
// Каждый бит соответствует элементу матрицы (1 - есть связь, 0 - нет связи).
// В случае ошибки записывает описание в параметр error.
bool unpackIncidenceMatrix(uint16_t vertexCount,
                           uint16_t edgeCount,
                           const std::vector<uint8_t>& bits,
                           std::vector<std::vector<int>>& matrix,
                           std::string& error);

// Упаковка матрицы инцидентности в битовый массив для компактной передачи.
// Каждый элемент матрицы кодируется одним битом (1 - есть связь, 0 - нет связи).
std::vector<uint8_t> packIncidenceMatrix(const std::vector<std::vector<int>>& matrix);

}  // namespace netproto


