#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <csignal>

volatile sig_atomic_t running = 1;

void handle_sigint(int) {
    running = 0;
    std::cout << "\n[Observer] Завершение по Ctrl+C\n";
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    const char *fifo_path = "/tmp/treasure_fifo";
    signal(SIGINT, handle_sigint);

    mkfifo(fifo_path, 0666); // создать, если нет

    int fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("open fifo");
        return 1;
    }

    std::cout << "[Observer] Наблюдатель запущен. Ожидание сообщений...\n";

    char buf[256];
    while (running) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << buf;
        } else {
            usleep(200000); // небольшая пауза
        }
    }

    close(fd);
    unlink(fifo_path);
    std::cout << "[Observer] FIFO закрыт и удалён.\n";
}
