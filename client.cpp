// Клиентская часть приложения: ввод графа, формирование запросов и обмен с сервером по TCP/UDP.

#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "graph.hpp"
#include "protocol.hpp"

namespace {

constexpr uint16_t kMaxVertices = 65535;
constexpr uint16_t kMaxEdges = 65535;
constexpr int kAckTimeoutSeconds = 3;
constexpr int kAckRetries = 3;

enum class Transport { Tcp, Udp };

struct ClientConfig {
    std::string ip;
    Transport transport;
    uint16_t port;
};

struct TcpConnection {
    int socket = -1;
    sockaddr_in address{};
};

struct UdpConnection {
    int socket = -1;
    sockaddr_in address{};
    uint16_t requestCounter = 1;
};

struct ClientState {
    graph::GraphDefinition graph;
    bool graphLoaded = false;
};

// Вывод краткой справки по командам клиента: показывает список доступных команд и их описание.
void printLocalHelp() {
    std::cout << "Доступные команды:\n"
                 "  help                - запросить список команд у сервера\n"
                 "  input               - ввести граф вручную\n"
                 "  load <путь>         - считать граф из файла\n"
                 "  query <u> <v>       - найти путь между вершинами u и v (нумерация с 0)\n"
                 "  exit                - завершить работу клиента\n";
}

// Чтение графа из потока: парсит данные графа из входного потока (файл или консоль).
// Формат: количество вершин, количество рёбер, матрица инцидентности (вершины x рёбра),
// список весов рёбер. В случае ошибки записывает описание в параметр error и возвращает false.
bool readGraphFromStream(std::istream& in, graph::GraphDefinition& graphDef, std::string& error) {
    uint32_t vertices = 0;
    uint32_t edges = 0;
    if (!(in >> vertices >> edges)) {
        error = "Не удалось прочитать размеры графа.";
        return false;
    }
    if (vertices < 6 || vertices > kMaxVertices) {
        error = "Неверное количество вершин: " + std::to_string(vertices) + 
                ". Требуется от 6 до " + std::to_string(kMaxVertices) + ".";
        return false;
    }
    if (edges < 6 || edges > kMaxEdges) {
        error = "Неверное количество рёбер: " + std::to_string(edges) + 
                ". Требуется от 6 до " + std::to_string(kMaxEdges) + ".";
        return false;
    }
    
    // Пропускаем оставшуюся часть строки с размерами (если есть) и переходим к следующей строке
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    graphDef.vertexCount = vertices;
    graphDef.edgeCount = edges;
    graphDef.incidence.assign(vertices, std::vector<int>(edges, 0));

    // Читаем матрицу инцидентности построчно с проверкой количества чисел в каждой строке
    for (uint16_t v = 0; v < vertices; ++v) {
        // Сохраняем позицию перед чтением строки для проверки лишних чисел
        std::string line;
        std::getline(in, line);
        if (in.fail() && !in.eof()) {
            error = "Ошибка при чтении строки " + std::to_string(v + 1) + " матрицы инцидентности.";
            return false;
        }
        
        // Парсим строку и подсчитываем количество чисел
        std::istringstream lineStream(line);
        std::vector<int> rowValues;
        int value = 0;
        while (lineStream >> value) {
            if (value != 0 && value != 1) {
                error = "Некорректное значение в матрице инцидентности (строка " + 
                        std::to_string(v + 1) + "): ожидается 0 или 1, получено " + 
                        std::to_string(value) + ".";
                return false;
            }
            rowValues.push_back(value);
        }
        
        // Проверяем количество чисел в строке
        if (rowValues.size() != edges) {
            error = "В строке " + std::to_string(v + 1) + 
                    " матрицы инцидентности неверное количество чисел: ожидается " + 
                    std::to_string(edges) + ", получено " + std::to_string(rowValues.size()) + ".";
            return false;
        }
        
        // Копируем значения в матрицу
        for (uint16_t e = 0; e < edges; ++e) {
            graphDef.incidence[v][e] = rowValues[e];
        }
    }

    // Читаем строку с весами
    std::string weightsLine;
    std::getline(in, weightsLine);
    if (in.fail() && !in.eof()) {
        error = "Ошибка при чтении строки с весами рёбер.";
        return false;
    }
    
    // Парсим веса и проверяем количество
    std::istringstream weightsStream(weightsLine);
    std::vector<uint32_t> weights;
    uint32_t weight = 0;
    while (weightsStream >> weight) {
        weights.push_back(weight);
    }
    
    // Проверяем количество весов
    if (weights.size() != edges) {
        error = "Неверное количество весов: ожидается " + 
                std::to_string(edges) + ", получено " + std::to_string(weights.size()) + ".";
        return false;
    }
    
    graphDef.weights = weights;
    
    // Проверяем, что после чтения всех данных не осталось лишних строк
    std::string extraLine;
    if (std::getline(in, extraLine)) {
        // Удаляем пробелы из строки
        extraLine.erase(0, extraLine.find_first_not_of(" \t\r\n"));
        extraLine.erase(extraLine.find_last_not_of(" \t\r\n") + 1);
        if (!extraLine.empty()) {
            error = "Обнаружены лишние данные после списка весов.";
            return false;
        }
    }
    // Очищаем флаги ошибок потока (EOF - это нормально)
    in.clear();
    
    return true;
}

// Ввод графа с консоли: запрашивает у пользователя данные графа и читает их построчно.
// Пользователь вводит данные в формате: вершины, рёбра, матрица инцидентности, веса.
// Пустая строка завершает ввод. Возвращает false при ошибке ввода.
bool inputGraphFromConsole(graph::GraphDefinition& graphDef) {
    std::cout << "Формат ввода:\n"
                 "  <вершины> <ребра>\n"
                 "  матрица инцидентности (вершины x ребра, значения 0/1)\n"
                 "  список весов (по одному числу на ребро)\n";
    std::cout << "Введите данные:\n";
    std::string line;
    std::stringstream buffer;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            break;
        }
        buffer << line << '\n';
    }

    std::string error;
    if (!readGraphFromStream(buffer, graphDef, error)) {
        std::cerr << "Ошибка ввода: " << error << "\n";
        return false;
    }
    return true;
}

// Загрузка графа из файла: открывает файл и читает из него данные графа.
// Использует readGraphFromStream для парсинга данных. Возвращает false при ошибке.
bool loadGraphFromFile(const std::string& path, graph::GraphDefinition& graphDef) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Не удалось открыть файл: " << path << "\n";
        return false;
    }
    std::string error;
    if (!readGraphFromStream(file, graphDef, error)) {
        std::cerr << "Ошибка чтения файла: " << error << "\n";
        return false;
    }
    return true;
}

// Простая проверка на стороне клиента: только размеры графа (количество вершин и рёбер).
// Возвращает false и описание ошибки в параметре error, если проверка не пройдена.
bool validateCounts(const graph::GraphDefinition& graphDef, std::string& error) {
    if (graphDef.vertexCount < 6) {
        error = "Граф должен содержать не менее 6 вершин.";
        return false;
    }
    if (graphDef.vertexCount > kMaxVertices) {
        error = "Граф должен содержать не более 65536 вершин.";
        return false;
    }
    if (graphDef.edgeCount < 6) {
        error = "Граф должен содержать не менее 6 рёбер.";
        return false;
    }
    if (graphDef.edgeCount > kMaxEdges) {
        error = "Граф должен содержать не более 65536 рёбер.";
        return false;
    }
    return true;
}

// Отправка всех данных через TCP-сокет: гарантирует отправку всех байтов, даже если send() отправляет частично.
// Выполняет повторные вызовы send() до тех пор, пока все данные не будут отправлены.
// Возвращает false при ошибке отправки.
bool sendAll(int socket, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t chunk = send(socket,
                             reinterpret_cast<const char*>(data) + sent,
                             static_cast<int>(size - sent),
                             0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

// Приём точного количества байтов через TCP-сокет: гарантирует получение всех запрошенных байтов.
// Выполняет повторные вызовы recv() до тех пор, пока не будет получено нужное количество байтов.
// Возвращает false при ошибке приёма или разрыве соединения.
bool recvExact(int socket, uint8_t* buffer, size_t size) {
    size_t received = 0;
    while (received < size) {
        ssize_t chunk = recv(socket,
                             reinterpret_cast<char*>(buffer) + received,
                             static_cast<int>(size - received),
                             0);
        if (chunk <= 0) {
            return false;
        }
        received += static_cast<size_t>(chunk);
    }
    return true;
}

// Чтение TCP-сообщения: получает заголовок и полезную нагрузку из TCP-сокета.
// Сначала читает заголовок фиксированного размера, затем полезную нагрузку указанного размера.
// Возвращает false при ошибке чтения или разрыве соединения.
bool readTcpMessage(int socket,
                    netproto::MessageHeader& header,
                    std::vector<uint8_t>& payload) {
    std::vector<uint8_t> headerBuf(netproto::kHeaderSize);
    if (!recvExact(socket, headerBuf.data(), headerBuf.size())) {
        return false;
    }
    if (!netproto::deserializeHeader(headerBuf, header)) {
        std::cerr << "Получен битый заголовок от сервера.\n";
        return false;
    }
    payload.resize(header.payloadSize);
    if (header.payloadSize > 0) {
        if (!recvExact(socket, payload.data(), payload.size())) {
            return false;
        }
    }
    return true;
}

// Отправка TCP-сообщения: отправляет заголовок и полезную нагрузку через TCP-сокет.
// Сначала отправляет заголовок, затем полезную нагрузку. Возвращает false при ошибке отправки.
bool sendTcpMessage(int socket,
                    const netproto::MessageHeader& header,
                    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> headerBuf = netproto::serializeHeader(header);
    if (!sendAll(socket, headerBuf.data(), headerBuf.size())) {
        return false;
    }
    if (!payload.empty()) {
        return sendAll(socket, payload.data(), payload.size());
    }
    return true;
}

// Отправка UDP-сообщения с подтверждением: реализует надёжную доставку для UDP.
// Отправляет сообщение и ждёт подтверждения (ACK) от сервера. Выполняет до 3 попыток с таймаутом 3 секунды.
// После получения ACK ожидает ответное сообщение с данными. Возвращает nullopt при потере связи.
std::optional<std::pair<netproto::MessageHeader, std::vector<uint8_t>>>
sendUdpWithAck(UdpConnection& connection,
               const netproto::MessageHeader& header,
               const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> headerBuf = netproto::serializeHeader(header);
    std::vector<uint8_t> packet;
    packet.reserve(headerBuf.size() + payload.size());
    packet.insert(packet.end(), headerBuf.begin(), headerBuf.end());
    packet.insert(packet.end(), payload.begin(), payload.end());

    for (int attempt = 1; attempt <= kAckRetries; ++attempt) {
        ssize_t sent = sendto(connection.socket,
                              packet.data(),
                              static_cast<int>(packet.size()),
                              0,
                              reinterpret_cast<sockaddr*>(&connection.address),
                              sizeof(connection.address));
        if (sent < 0) {
            perror("sendto");
            return std::nullopt;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(connection.socket, &readSet);
        timeval timeout{};
        timeout.tv_sec = kAckTimeoutSeconds;

        int ready = select(connection.socket + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready > 0 && FD_ISSET(connection.socket, &readSet)) {
            std::vector<uint8_t> recvBuf(2048);
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            ssize_t bytes = recvfrom(connection.socket,
                                     recvBuf.data(),
                                     static_cast<int>(recvBuf.size()),
                                     0,
                                     reinterpret_cast<sockaddr*>(&from),
                                     &fromLen);
            if (bytes < static_cast<ssize_t>(netproto::kHeaderSize)) {
                continue;
            }
            recvBuf.resize(static_cast<size_t>(bytes));
            std::vector<uint8_t> headerPart(recvBuf.begin(),
                                            recvBuf.begin() + netproto::kHeaderSize);
            netproto::MessageHeader ackHeader;
            if (!netproto::deserializeHeader(headerPart, ackHeader)) {
                continue;
            }
            std::vector<uint8_t> payloadPart(recvBuf.begin() + netproto::kHeaderSize, recvBuf.end());

            if (ackHeader.command == netproto::Command::Ack &&
                ackHeader.requestId == header.requestId) {
                if (payloadPart.empty()) {
                    // Ждём ответ с данными.
                    FD_ZERO(&readSet);
                    FD_SET(connection.socket, &readSet);
                    timeout.tv_sec = kAckTimeoutSeconds;
                    int readyResp = select(connection.socket + 1, &readSet, nullptr, nullptr, &timeout);
                    if (readyResp > 0 && FD_ISSET(connection.socket, &readSet)) {
                        std::vector<uint8_t> respBuf(65536);
                        sockaddr_in respFrom{};
                        socklen_t respLen = sizeof(respFrom);
                        ssize_t respBytes = recvfrom(connection.socket,
                                                     respBuf.data(),
                                                     static_cast<int>(respBuf.size()),
                                                     0,
                                                     reinterpret_cast<sockaddr*>(&respFrom),
                                                     &respLen);
                        if (respBytes < static_cast<ssize_t>(netproto::kHeaderSize)) {
                            continue;
                        }
                        respBuf.resize(static_cast<size_t>(respBytes));
                        std::vector<uint8_t> respHeaderPart(respBuf.begin(),
                                                            respBuf.begin() + netproto::kHeaderSize);
                        netproto::MessageHeader responseHeader;
                        if (!netproto::deserializeHeader(respHeaderPart, responseHeader)) {
                            continue;
                        }
                        std::vector<uint8_t> respPayload(respBuf.begin() + netproto::kHeaderSize,
                                                         respBuf.end());
                        return std::make_optional(std::make_pair(responseHeader, respPayload));
                    }
                }
            } else {
                // Возможно, пришёл ответ без ACK (например, help).
                if (ackHeader.requestId == header.requestId) {
                    return std::make_optional(std::make_pair(ackHeader, payloadPart));
                }
            }
        } else {
            std::cout << "(Нет ответа, попытка " << attempt << ")\n";
        }
    }
    std::cout << "Потеряна связь с сервером.\n";
    return std::nullopt;
}

// Обработка ошибки от сервера: десериализует и выводит сообщение об ошибке.
void handleServerError(const netproto::MessageHeader& header,
                       const std::vector<uint8_t>& payload) {
    std::string message;
    if (netproto::deserializeString(payload, message)) {
        std::cerr << "Ошибка сервера: " << message << "\n";
    } else {
        std::cerr << "Сервер вернул ошибку без описания.\n";
    }
}

// Обработка результата поиска пути: десериализует и выводит длину пути и последовательность вершин.
void handlePathResult(const std::vector<uint8_t>& payload) {
    netproto::PathResultPayload resultPayload;
    std::string error;
    if (!netproto::deserializePathResult(payload, resultPayload, error)) {
        std::cerr << "Не удалось разобрать ответ пути: " << error << "\n";
        return;
    }
    std::cout << "Длина пути: " << resultPayload.distance << "\nПуть: ";
    for (size_t i = 0; i < resultPayload.path.size(); ++i) {
        std::cout << resultPayload.path[i];
        if (i + 1 < resultPayload.path.size()) {
            std::cout << " -> ";
        }
    }
    std::cout << "\n";
}

// Построение полезной нагрузки для загрузки графа: упаковывает граф в бинарный формат протокола.
std::vector<uint8_t> buildUploadPayload(const graph::GraphDefinition& graphDef) {
    netproto::UploadGraphPayload payload;
    payload.vertexCount = graphDef.vertexCount;
    payload.edgeCount = graphDef.edgeCount;
    payload.incidenceBits = netproto::packIncidenceMatrix(graphDef.incidence);
    payload.weights = graphDef.weights;
    return netproto::serializeUploadGraph(payload);
}

// Обработка ответа от сервера: определяет тип команды и вызывает соответствующую функцию обработки.
// Поддерживает команды: Error, Help, PathResult, Ack, UploadGraph.
void processResponse(const netproto::MessageHeader& header,
                     const std::vector<uint8_t>& payload) {
    if (header.command == netproto::Command::Error) {
        handleServerError(header, payload);
        return;
    }
    if (header.command == netproto::Command::Help) {
        std::string text;
        if (netproto::deserializeString(payload, text)) {
            std::cout << text << "\n";
        } else {
            std::cout << "Справка получена, но не удалось её прочитать.\n";
        }
        return;
    }
    if (header.command == netproto::Command::PathResult) {
        handlePathResult(payload);
        return;
    }
    if (header.command == netproto::Command::Ack) {
        std::cout << "Получено подтверждение.\n";
        return;
    }
    if (header.command == netproto::Command::UploadGraph) {
        // Сервер подтвердил загрузку графа
        std::string text;
        if (netproto::deserializeString(payload, text)) {
            std::cout << text << "\n";
        }
        return;
    }
    std::cout << "Сервер вернул неизвестную команду.\n";
}

// Запуск TCP-клиента: устанавливает соединение с сервером и обрабатывает команды пользователя.
// Поддерживает команды: help, input, load, query, exit.
// Для каждой команды отправляет соответствующий запрос серверу и обрабатывает ответ.
void runTcpClient(const ClientConfig& config) {
    TcpConnection connection;
    connection.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (connection.socket < 0) {
        perror("socket");
        return;
    }
    connection.address.sin_family = AF_INET;
    connection.address.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.ip.c_str(), &connection.address.sin_addr) <= 0) {
        std::cerr << "Неверный IP-адрес.\n";
        close(connection.socket);
        return;
    }

    if (connect(connection.socket,
                reinterpret_cast<sockaddr*>(&connection.address),
                sizeof(connection.address)) < 0) {
        perror("connect");
        close(connection.socket);
        return;
    }

    std::cout << "Подключено к TCP серверу " << config.ip << ":" << config.port << "\n";
    ClientState state;

    while (true) {
        std::cout << "> ";
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line.empty()) {
            continue;
        }
        std::istringstream cmd(line);
        std::string command;
        cmd >> command;

        if (command == "help") {
            // Выводим локальную справку по командам клиента
            printLocalHelp();
        } else if (command == "input") {
            graph::GraphDefinition graphDef;
            if (!inputGraphFromConsole(graphDef)) {
                continue;
            }
            std::string validationErr;
            if (!validateCounts(graphDef, validationErr)) {
                std::cerr << "Валидация не пройдена: " << validationErr << "\n";
                continue;
            }
            std::vector<uint8_t> payload = buildUploadPayload(graphDef);
            netproto::MessageHeader header{netproto::Command::UploadGraph,
                                           netproto::Status::Ok,
                                           0,
                                           static_cast<uint32_t>(payload.size()),
                                           0};
            if (!sendTcpMessage(connection.socket, header, payload)) {
                std::cerr << "Ошибка при отправке графа.\n";
                break;
            }
            netproto::MessageHeader responseHeader;
            std::vector<uint8_t> responsePayload;
            if (!readTcpMessage(connection.socket, responseHeader, responsePayload)) {
                std::cerr << "Соединение с сервером разорвано.\n";
                break;
            }
            if (responseHeader.status == netproto::Status::Ok) {
                state.graph = graphDef;
                state.graphLoaded = true;
                std::cout << "Граф успешно загружен на сервер.\n";
            }
            processResponse(responseHeader, responsePayload);
        } else if (command == "load") {
            std::string path;
            cmd >> path;
            if (path.empty()) {
                std::cerr << "Укажите путь к файлу.\n";
                continue;
            }
            graph::GraphDefinition graphDef;
            if (!loadGraphFromFile(path, graphDef)) {
                continue;
            }
            std::string validationErr;
            if (!validateCounts(graphDef, validationErr)) {
                std::cerr << "Валидация не пройдена: " << validationErr << "\n";
                continue;
            }
            std::vector<uint8_t> payload = buildUploadPayload(graphDef);
            netproto::MessageHeader header{netproto::Command::UploadGraph,
                                           netproto::Status::Ok,
                                           0,
                                           static_cast<uint32_t>(payload.size()),
                                           0};
            if (!sendTcpMessage(connection.socket, header, payload)) {
                std::cerr << "Ошибка при отправке графа.\n";
                break;
            }
            netproto::MessageHeader responseHeader;
            std::vector<uint8_t> responsePayload;
            if (!readTcpMessage(connection.socket, responseHeader, responsePayload)) {
                std::cerr << "Соединение с сервером разорвано.\n";
                break;
            }
            if (responseHeader.status == netproto::Status::Ok) {
                state.graph = graphDef;
                state.graphLoaded = true;
                std::cout << "Граф успешно загружен на сервер.\n";
            }
            processResponse(responseHeader, responsePayload);
        } else if (command == "query") {
            int source = -1;
            int target = -1;
            cmd >> source >> target;
            if (source < 0 || target < 0) {
                std::cerr << "Укажите вершины в формате: query <u> <v>.\n";
                continue;
            }
            if (!state.graphLoaded) {
                std::cerr << "Сначала загрузите граф (команды input/load).\n";
                continue;
            }
            if (source >= state.graph.vertexCount || target >= state.graph.vertexCount) {
                std::cerr << "Вершины вне диапазона [0, "
                          << state.graph.vertexCount - 1 << "].\n";
                continue;
            }
            netproto::PathQueryPayload queryPayload{static_cast<uint16_t>(source),
                                                    static_cast<uint16_t>(target)};
            std::vector<uint8_t> payload = netproto::serializePathQuery(queryPayload);
            netproto::MessageHeader header{netproto::Command::PathQuery,
                                           netproto::Status::Ok,
                                           0,
                                           static_cast<uint32_t>(payload.size()),
                                           0};
            if (!sendTcpMessage(connection.socket, header, payload)) {
                std::cerr << "Ошибка отправки запроса пути.\n";
                break;
            }
            netproto::MessageHeader responseHeader;
            std::vector<uint8_t> responsePayload;
            if (!readTcpMessage(connection.socket, responseHeader, responsePayload)) {
                std::cerr << "Соединение с сервером разорвано.\n";
                break;
            }
            processResponse(responseHeader, responsePayload);
        } else if (command == "exit") {
            netproto::MessageHeader header{netproto::Command::Exit,
                                           netproto::Status::Ok,
                                           0,
                                           0,
                                           0};
            sendTcpMessage(connection.socket, header, {});
            std::cout << "Завершение работы клиента.\n";
            break;
        } else {
            std::cout << "Неизвестная команда. Используйте help для списка команд.\n";
        }
    }

    close(connection.socket);
}

// Запуск UDP-клиента: создаёт UDP-сокет и обрабатывает команды пользователя с надёжной доставкой.
// Поддерживает те же команды, что и TCP-клиент, но использует механизм подтверждений (ACK).
// Для каждой команды отправляет запрос с уникальным requestId и ждёт подтверждения и ответа.
void runUdpClient(const ClientConfig& config) {
    UdpConnection connection;
    connection.socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (connection.socket < 0) {
        perror("socket");
        return;
    }
    connection.address.sin_family = AF_INET;
    connection.address.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.ip.c_str(), &connection.address.sin_addr) <= 0) {
        std::cerr << "Неверный IP-адрес.\n";
        close(connection.socket);
        return;
    }
    std::cout << "Подключено к UDP серверу " << config.ip << ":" << config.port << "\n";

    ClientState state;

    while (true) {
        std::cout << "> ";
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line.empty()) {
            continue;
        }
        std::istringstream cmd(line);
        std::string command;
        cmd >> command;

        auto nextRequestId = [&connection]() {
            return connection.requestCounter++;
        };

        if (command == "help") {
            // Выводим локальную справку по командам клиента
            printLocalHelp();
        } else if (command == "input") {
            graph::GraphDefinition graphDef;
            if (!inputGraphFromConsole(graphDef)) {
                continue;
            }
            std::string validationErr;
            if (!validateCounts(graphDef, validationErr)) {
                std::cerr << "Валидация не пройдена: " << validationErr << "\n";
                continue;
            }
            std::vector<uint8_t> payload = buildUploadPayload(graphDef);
            netproto::MessageHeader header{netproto::Command::UploadGraph,
                                           netproto::Status::Ok,
                                           nextRequestId(),
                                           static_cast<uint32_t>(payload.size()),
                                           0};
            auto response = sendUdpWithAck(connection, header, payload);
            if (response) {
                if (response->first.status == netproto::Status::Ok) {
                    state.graph = graphDef;
                    state.graphLoaded = true;
                    std::cout << "Граф успешно загружен на сервер.\n";
                }
                processResponse(response->first, response->second);
                } else {
                break;
            }
        } else if (command == "load") {
            std::string path;
            cmd >> path;
            if (path.empty()) {
                std::cerr << "Укажите путь к файлу.\n";
                continue;
            }
            graph::GraphDefinition graphDef;
            if (!loadGraphFromFile(path, graphDef)) {
                continue;
            }
            std::string validationErr;
            if (!validateCounts(graphDef, validationErr)) {
                std::cerr << "Валидация не пройдена: " << validationErr << "\n";
                continue;
            }
            std::vector<uint8_t> payload = buildUploadPayload(graphDef);
            netproto::MessageHeader header{netproto::Command::UploadGraph,
                                           netproto::Status::Ok,
                                           nextRequestId(),
                                           static_cast<uint32_t>(payload.size()),
                                           0};
            auto response = sendUdpWithAck(connection, header, payload);
            if (response) {
                if (response->first.status == netproto::Status::Ok) {
                    state.graph = graphDef;
                    state.graphLoaded = true;
                    std::cout << "Граф успешно загружен на сервер.\n";
                }
                processResponse(response->first, response->second);
            } else {
                break;
            }
        } else if (command == "query") {
            int source = -1;
            int target = -1;
            cmd >> source >> target;
            if (source < 0 || target < 0) {
                std::cerr << "Укажите вершины в формате: query <u> <v>.\n";
                continue;
            }
            if (!state.graphLoaded) {
                std::cerr << "Сначала загрузите граф (команды input/load).\n";
                continue;
            }
            if (source >= state.graph.vertexCount || target >= state.graph.vertexCount) {
                std::cerr << "Вершины вне диапазона [0, "
                          << state.graph.vertexCount - 1 << "].\n";
                continue;
            }
            netproto::PathQueryPayload queryPayload{static_cast<uint16_t>(source),
                                                    static_cast<uint16_t>(target)};
            std::vector<uint8_t> payload = netproto::serializePathQuery(queryPayload);
            netproto::MessageHeader header{netproto::Command::PathQuery,
                                           netproto::Status::Ok,
                                           nextRequestId(),
                                           static_cast<uint32_t>(payload.size()),
                                           0};
            auto response = sendUdpWithAck(connection, header, payload);
            if (response) {
                processResponse(response->first, response->second);
            } else {
                break;
            }
        } else if (command == "exit") {
            netproto::MessageHeader header{netproto::Command::Exit,
                                           netproto::Status::Ok,
                                           nextRequestId(),
                                           0,
                                           0};
            sendUdpWithAck(connection, header, {});
            std::cout << "Завершение работы клиента.\n";
            break;
        } else {
            std::cout << "Неизвестная команда. Используйте help для списка команд.\n";
        }
    }

    close(connection.socket);
}

// Парсинг аргументов командной строки: извлекает IP-адрес, протокол (tcp/udp) и порт.
// Возвращает nullopt при некорректных аргументах.
std::optional<ClientConfig> parseArguments(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Использование: " << argv[0] << " <ip> <protocol> <port>\n";
        return std::nullopt;
    }
    ClientConfig config;
    config.ip = argv[1];
    std::string proto = argv[2];
    if (proto == "tcp") {
        config.transport = Transport::Tcp;
    } else if (proto == "udp") {
        config.transport = Transport::Udp;
    } else {
        std::cerr << "Неизвестный протокол: " << proto << "\n";
        return std::nullopt;
    }
    int port = std::stoi(argv[3]);
    if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
        std::cerr << "Некорректный порт.\n";
        return std::nullopt;
    }
    config.port = static_cast<uint16_t>(port);
    return config;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto configOpt = parseArguments(argc, argv);
    if (!configOpt) {
        return 1;
    }
    printLocalHelp();

    const ClientConfig& config = *configOpt;
    if (config.transport == Transport::Tcp) {
        runTcpClient(config);
    } else {
        runUdpClient(config);
    }
    return 0;
}


