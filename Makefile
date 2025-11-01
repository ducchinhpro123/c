CC = gcc
CFLAGS = -Wall -Wextra -I/usr/include
LIBS = -lm -lraylib -lraygui -lpthread -ldl -lrt -lX11

SERVER_TARGET = server_gui
SERVER_SOURCE = server_gui.c server.c message.c


CLIENT_TARGET = client_gui
CLIENT_SOURCE = client_gui.c client.c warning_dialog.c client_network.c message.c

all: $(SERVER_TARGET)

$(SERVER_TARGET): $(SERVER_SOURCE)
	$(CC) $(CFLAGS) $(SERVER_SOURCE) $(LIBS) -o $(SERVER_TARGET) $(LIBS)

$(CLIENT_TARGET): $(CLIENT_SOURCE)
	$(CC) $(CFLAGS) $(CLIENT_SOURCE) $(LIBS) -o $(CLIENT_TARGET) $(LIBS)

server: $(SERVER_TARGET)

client: $(CLIENT_TARGET)

clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET)
