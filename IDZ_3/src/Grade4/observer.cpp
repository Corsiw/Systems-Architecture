#include <iostream>
#include <string>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

using namespace std;

static volatile sig_atomic_t g_stop = 0;
void sigint_handler(int) {
    g_stop = 1;
}

int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cout.setf(std::ios::unitbuf);
    setvbuf(stdout, nullptr, _IONBF, 0);

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    // Уникальное имя FIFO
    pid_t pid = getpid();
    string fifo_name = "/tmp/treasure_observer_fifo_" + to_string(pid);

    // Создание FIFO
    if (mkfifo(fifo_name.c_str(), 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo");
            return 1;
        }
    }

    cout << "[Observer pid=" << pid << "] создан канал: " << fifo_name << endl;
    cout << "Ожидаю сообщения...\n";

    int fd = open(fifo_name.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("open fifo");
        unlink(fifo_name.c_str());
        return 1;
    }

    char buf[512];
    while (!g_stop) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << buf;
        } else {
            usleep(200000); // небольшая пауза
        }
    }

    cout << "\n[Observer pid=" << pid << "] Завершаюсь...\n";
    close(fd);
    unlink(fifo_name.c_str());
    return 0;
}
