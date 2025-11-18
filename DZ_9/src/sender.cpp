#include <csignal>
#include <iostream>
#include <unistd.h>

void wait_ack() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    siginfo_t info;
    sigwaitinfo(&set, &info);  // ждём ACK
}

void send_bit(pid_t pid, int bit) {
    int sig = bit ? SIGUSR2 : SIGUSR1;
    kill(pid, sig);
    wait_ack();
}

void send_int64(pid_t pid, int64_t value) {
    for (int i = 63; i >= 0; --i) {
        send_bit(pid, (value >> i) & 1);
    }
}

int main() {
    // Блокируем SIGUSR1 заранее
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, nullptr);

    std::cout << "Sender PID: " << getpid() << "\n";

    pid_t receiver_pid;
    std::cout << "Enter Receiver PID: ";
    std::cin >> receiver_pid;

    // Ждём стартовый ACK → receiver готов
    wait_ack();

    std::cout << "Enter integer: ";
    int64_t num;
    std::cin >> num;

    // Передаём данные
    send_int64(receiver_pid, num);

    std::cout << "Done\n";
}
