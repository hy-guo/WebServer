SOURCE = main.cpp http_conn.cpp thread_pool.cpp

FLAGS = -pthread

web_server.out: $(SOURCE)
    g++ $(SOURCE) $(FLAGS) -o web_werver.out