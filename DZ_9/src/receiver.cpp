#include <csignal>
#include <iostream>
#include <unistd.h>

volatile sig_atomic_t bit = -1;
volatile sig_atomic_t ready = 0;
pid_t sender_pid = -1;

void handler(int sig, siginfo_t* info, void*) {
    if (!ready) return;                 // ещё не готовы принимать

    pid_t pid = info->si_pid;           // узнаём PID отправителя
    if (pid != sender_pid) {
      return;
    }

    bit = (sig == SIGUSR2 ? 1 : 0);

    kill(sender_pid, SIGUSR1);          // ACK
}

int main() {

    // Блокируем SIGUSR1/SIGUSR2 на время установки обработчиков
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, nullptr);

    std::cout << "Receiver PID: " << getpid() << "\n";

    std::cout << "Enter Sender PID: ";
    std::cin >> sender_pid;

    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGUSR2, &sa, nullptr);

    // Теперь можно принимать сигналы
    sigprocmask(SIG_UNBLOCK, &set, nullptr);
    ready = 1;

    // Отправляем первый ACK
    kill(sender_pid, SIGUSR1);

    int64_t result = 0;
    int bit_count = 0;

    while (bit_count < 64) {
        pause();

        if (bit == -1)
            continue;

        result = (result << 1) | bit;
        bit = -1;
        bit_count++;
    }

    std::cout << "Received: " << result << "\n";
}
