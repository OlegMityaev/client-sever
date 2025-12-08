g++ client.cpp protocol.cpp graph.cpp -o client

./client 127.0.0.1 tcp 8080 

g++ server.cpp protocol.cpp graph.cpp -o server -pthread

./server tcp 8080

