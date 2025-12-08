// Реализация функций сериализации и утилит сетевого протокола.

#include "protocol.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace netproto {

namespace {

// Максимальный размер полезной нагрузки (1 MiB) для защиты от слишком больших сообщений.
constexpr uint32_t kMaxPayloadSize = 1 << 20;

// Вспомогательная функция: добавляет целочисленное значение в буфер в сетевом порядке (big endian).
// Поддерживает типы размером 1, 2 и 4 байта.
template <typename T>
void appendBytes(std::vector<uint8_t>& buffer, T value) {
    static_assert(std::is_integral_v<T>, "Integral type required");
    if constexpr (sizeof(T) == 1) {
        buffer.push_back(static_cast<uint8_t>(value));
    } else if constexpr (sizeof(T) == 2) {
        uint16_t netValue = htons(static_cast<uint16_t>(value));
        uint8_t raw[2];
        std::memcpy(raw, &netValue, sizeof(raw));
        buffer.insert(buffer.end(), raw, raw + sizeof(raw));
    } else if constexpr (sizeof(T) == 4) {
        uint32_t netValue = htonl(static_cast<uint32_t>(value));
        uint8_t raw[4];
        std::memcpy(raw, &netValue, sizeof(raw));
        buffer.insert(buffer.end(), raw, raw + sizeof(raw));
    } else {
        static_assert(sizeof(T) <= 4, "Unsupported integral size");
    }
}

// Вспомогательная функция: читает целочисленное значение из буфера в сетевом порядке (big endian).
// Поддерживает типы размером 1, 2 и 4 байта. Смещение offset увеличивается после чтения.
template <typename T>
bool readBytes(const std::vector<uint8_t>& buffer, std::size_t& offset, T& value) {
    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }
    if constexpr (sizeof(T) == 1) {
        value = static_cast<T>(buffer[offset]);
    } else if constexpr (sizeof(T) == 2) {
        uint16_t raw = 0;
        std::memcpy(&raw, buffer.data() + offset, sizeof(raw));
        value = static_cast<T>(ntohs(raw));
    } else if constexpr (sizeof(T) == 4) {
        uint32_t raw = 0;
        std::memcpy(&raw, buffer.data() + offset, sizeof(raw));
        value = static_cast<T>(ntohl(raw));
    } else {
        return false;
    }
    offset += sizeof(T);
    return true;
}

}  // namespace

// Сериализация заголовка сообщения: преобразует структуру MessageHeader в массив байтов.
// Все числовые поля конвертируются в сетевой порядок байтов (big endian).
std::vector<uint8_t> serializeHeader(const MessageHeader& header) {
    std::vector<uint8_t> buffer;
    buffer.reserve(kHeaderSize);
    appendBytes<uint8_t>(buffer, static_cast<uint8_t>(header.command));
    appendBytes<uint8_t>(buffer, static_cast<uint8_t>(header.status));
    appendBytes<uint16_t>(buffer, header.requestId);
    appendBytes<uint32_t>(buffer, header.payloadSize);
    appendBytes<uint32_t>(buffer, header.reserved);
    return buffer;
}

// Десериализация заголовка сообщения: восстанавливает структуру MessageHeader из массива байтов.
// Проверяет размер буфера и корректность данных. Возвращает false при ошибке.
bool deserializeHeader(const std::vector<uint8_t>& buffer, MessageHeader& header) {
    if (buffer.size() != kHeaderSize) {
        return false;
    }
    std::size_t offset = 0;
    uint8_t commandRaw = 0;
    uint8_t statusRaw = 0;

    if (!readBytes(buffer, offset, commandRaw) ||
        !readBytes(buffer, offset, statusRaw) ||
        !readBytes(buffer, offset, header.requestId) ||
        !readBytes(buffer, offset, header.payloadSize) ||
        !readBytes(buffer, offset, header.reserved)) {
        return false;
    }

    header.command = static_cast<Command>(commandRaw);
    header.status = static_cast<Status>(statusRaw);
    if (header.payloadSize > kMaxPayloadSize) {
        return false;
    }
    return true;
}

// Сериализация полезной нагрузки UploadGraph: упаковывает граф в бинарный формат.
// Формат: vertexCount (2 байта) + edgeCount (2 байта) + размер битов (4 байта) + биты матрицы + количество весов (4 байта) + веса.
std::vector<uint8_t> serializeUploadGraph(const UploadGraphPayload& payload) {
    std::vector<uint8_t> buffer;
    buffer.reserve(4 + payload.incidenceBits.size() + payload.weights.size() * sizeof(uint32_t));
    appendBytes<uint16_t>(buffer, payload.vertexCount);
    appendBytes<uint16_t>(buffer, payload.edgeCount);

    appendBytes<uint32_t>(buffer, static_cast<uint32_t>(payload.incidenceBits.size()));
    buffer.insert(buffer.end(), payload.incidenceBits.begin(), payload.incidenceBits.end());

    appendBytes<uint32_t>(buffer, static_cast<uint32_t>(payload.weights.size()));
    for (uint32_t weight : payload.weights) {
        appendBytes<uint32_t>(buffer, weight);
    }
    return buffer;
}

// Десериализация полезной нагрузки UploadGraph: восстанавливает граф из бинарного формата.
// Проверяет корректность размеров и соответствие количества весов количеству рёбер.
// В случае ошибки записывает описание в параметр error и возвращает false.
bool deserializeUploadGraph(const std::vector<uint8_t>& buffer,
                            UploadGraphPayload& payload,
                            std::string& error) {
    std::size_t offset = 0;
    uint32_t bitsSize = 0;
    uint32_t weightCount = 0;

    if (!readBytes(buffer, offset, payload.vertexCount) ||
        !readBytes(buffer, offset, payload.edgeCount) ||
        !readBytes(buffer, offset, bitsSize)) {
        error = "Заголовок поврежден.";
        return false;
    }

    if (offset + bitsSize > buffer.size()) {
        error = "Неверный размер блока бит матрицы инцидентности.";
        return false;
    }
    payload.incidenceBits.assign(buffer.begin() + offset, buffer.begin() + offset + bitsSize);
    offset += bitsSize;

    if (!readBytes(buffer, offset, weightCount)) {
        error = "Отсутствует блок весов.";
        return false;
    }

    if (weightCount != payload.edgeCount) {
        error = "Количество весов не совпадает с количеством рёбер.";
        return false;
    }

    if (offset + weightCount * sizeof(uint32_t) > buffer.size()) {
        error = "Недостаточно данных для списка весов.";
        return false;
    }

    payload.weights.clear();
    payload.weights.reserve(weightCount);
    for (uint32_t i = 0; i < weightCount; ++i) {
        uint32_t value = 0;
        if (!readBytes(buffer, offset, value)) {
            error = "Ошибка чтения веса ребра.";
            return false;
        }
        payload.weights.push_back(value);
    }

    if (offset != buffer.size()) {
        error = "Остались необработанные данные в полезной нагрузке.";
        return false;
    }

    return true;
}

// Сериализация полезной нагрузки PathQuery: упаковывает запрос пути в бинарный формат.
// Формат: source (2 байта) + target (2 байта).
std::vector<uint8_t> serializePathQuery(const PathQueryPayload& payload) {
    std::vector<uint8_t> buffer;
    buffer.reserve(4);
    appendBytes<uint16_t>(buffer, payload.source);
    appendBytes<uint16_t>(buffer, payload.target);
    return buffer;
}

// Десериализация полезной нагрузки PathQuery: восстанавливает запрос пути из бинарного формата.
// Проверяет, что размер буфера равен 4 байтам (2 байта на каждую вершину).
bool deserializePathQuery(const std::vector<uint8_t>& buffer, PathQueryPayload& payload) {
    if (buffer.size() != 4) {
        return false;
    }
    std::size_t offset = 0;
    return readBytes(buffer, offset, payload.source) &&
           readBytes(buffer, offset, payload.target);
}

// Сериализация полезной нагрузки PathResult: упаковывает результат поиска пути в бинарный формат.
// Формат: distance (4 байта) + длина пути (2 байта) + последовательность вершин (по 2 байта каждая).
std::vector<uint8_t> serializePathResult(const PathResultPayload& payload) {
    std::vector<uint8_t> buffer;
    buffer.reserve(4 + 2 + payload.path.size() * 2);
    appendBytes<uint32_t>(buffer, payload.distance);
    appendBytes<uint16_t>(buffer, static_cast<uint16_t>(payload.path.size()));
    for (uint16_t vertex : payload.path) {
        appendBytes<uint16_t>(buffer, vertex);
    }
    return buffer;
}

// Десериализация полезной нагрузки PathResult: восстанавливает результат из бинарного формата.
// Проверяет корректность размера буфера и читает последовательность вершин пути.
// В случае ошибки записывает описание в параметр error и возвращает false.
bool deserializePathResult(const std::vector<uint8_t>& buffer,
                           PathResultPayload& payload,
                           std::string& error) {
    std::size_t offset = 0;
    uint16_t pathSize = 0;

    if (!readBytes(buffer, offset, payload.distance) ||
        !readBytes(buffer, offset, pathSize)) {
        error = "Некорректный заголовок ответа пути.";
        return false;
    }
    if (offset + pathSize * sizeof(uint16_t) != buffer.size()) {
        error = "Неверный размер массива пути.";
        return false;
    }
    payload.path.clear();
    payload.path.reserve(pathSize);
    for (uint16_t i = 0; i < pathSize; ++i) {
        uint16_t vertex = 0;
        if (!readBytes(buffer, offset, vertex)) {
            error = "Ошибка чтения вершины пути.";
            return false;
        }
        payload.path.push_back(vertex);
    }
    return true;
}

// Сериализация строки: упаковывает строку в бинарный формат.
// Формат: длина строки (2 байта) + байты строки.
std::vector<uint8_t> serializeString(const std::string& text) {
    std::vector<uint8_t> buffer;
    buffer.reserve(2 + text.size());
    appendBytes<uint16_t>(buffer, static_cast<uint16_t>(text.size()));
    buffer.insert(buffer.end(), text.begin(), text.end());
    return buffer;
}

// Десериализация строки: восстанавливает строку из бинарного формата.
// Проверяет, что размер буфера соответствует заявленной длине строки.
bool deserializeString(const std::vector<uint8_t>& buffer, std::string& text) {
    std::size_t offset = 0;
    uint16_t size = 0;
    if (!readBytes(buffer, offset, size)) {
        return false;
    }
    if (offset + size != buffer.size()) {
        return false;
    }
    text.assign(reinterpret_cast<const char*>(buffer.data() + offset), size);
    return true;
}

// Упаковка матрицы инцидентности в битовый массив для компактной передачи.
// Каждый элемент матрицы кодируется одним битом: 1 - есть связь вершины с ребром, 0 - нет связи.
// Элементы матрицы упаковываются построчно (сначала все рёбра для первой вершины, затем для второй и т.д.).
std::vector<uint8_t> packIncidenceMatrix(const std::vector<std::vector<int>>& matrix) {
    if (matrix.empty() || matrix[0].empty()) {
        return {};
    }
    const uint16_t vertexCount = static_cast<uint16_t>(matrix.size());
    const uint16_t edgeCount = static_cast<uint16_t>(matrix[0].size());
    const std::size_t totalBits = static_cast<std::size_t>(vertexCount) * edgeCount;
    std::vector<uint8_t> bits((totalBits + 7) / 8, 0);

    std::size_t bitIndex = 0;
    for (uint16_t v = 0; v < vertexCount; ++v) {
        for (uint16_t e = 0; e < edgeCount; ++e) {
            if (matrix[v][e] != 0) {
                const std::size_t byteIndex = bitIndex / 8;
                const uint8_t bitPos = static_cast<uint8_t>(bitIndex % 8);
                bits[byteIndex] |= static_cast<uint8_t>(1u << bitPos);
            }
            ++bitIndex;
        }
    }
    return bits;
}

// Распаковка матрицы инцидентности из битового массива в двумерную матрицу.
// Каждый бит соответствует элементу матрицы: 1 - есть связь вершины с ребром, 0 - нет связи.
// Проверяет корректность размера битового массива. В случае ошибки записывает описание в параметр error.
bool unpackIncidenceMatrix(uint16_t vertexCount,
                           uint16_t edgeCount,
                           const std::vector<uint8_t>& bits,
                           std::vector<std::vector<int>>& matrix,
                           std::string& error) {
    if (vertexCount == 0 || edgeCount == 0) {
        error = "Пустая матрица.";
        return false;
    }
    const std::size_t totalBits = static_cast<std::size_t>(vertexCount) * edgeCount;
    const std::size_t expectedBytes = (totalBits + 7) / 8;
    if (bits.size() != expectedBytes) {
        error = "Несоответствие размера битового массива матрице.";
        return false;
    }
    matrix.assign(vertexCount, std::vector<int>(edgeCount, 0));
    std::size_t bitIndex = 0;
    for (uint16_t v = 0; v < vertexCount; ++v) {
        for (uint16_t e = 0; e < edgeCount; ++e) {
            const std::size_t byteIndex = bitIndex / 8;
            const uint8_t bitPos = static_cast<uint8_t>(bitIndex % 8);
            const bool set = (bits[byteIndex] >> bitPos) & 0x01u;
            matrix[v][e] = set ? 1 : 0;
            ++bitIndex;
        }
    }
    return true;
}

}  // namespace netproto


