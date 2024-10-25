compile-server:
	gcc server.c -o server
run-server:
	make compile-server && ./server
compile-client:
	gcc client.c -o client
run-client:
	make compile-client && ./client
clean:
	rm client server