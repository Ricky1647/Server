.PHONY: all clean

all: server client
server: server.cpp
	g++  -o server server.cpp ./utils/base64.cpp
client: client.cpp
	g++  -o client client.cpp ./utils/base64.cpp
clean:
	@rm -rf server client 
