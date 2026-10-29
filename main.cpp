#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using Number = std::uint64_t;
using Clock = std::chrono::steady_clock;

struct Task {
    Number a, b;
};

struct Result {
    Number a, b, gcd;
};

// Закрытая очередь отдаёт остаток данных; отменённая прекращает работу сразу.
template<class T>
class BlockingQueue {
    std::queue<T> data;
    const std::size_t capacity;
    std::mutex mutex;
    std::condition_variable changed;
    bool closed = false;
    bool cancelled = false;
    std::size_t peak = 0, producerWaits = 0, consumerWaits = 0;
public:
    explicit BlockingQueue(std::size_t size) : capacity(size) {
        if (size == 0) throw std::invalid_argument("Нулевая ёмкость очереди.");
    }

    bool push(const T& value) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!closed && !cancelled && data.size() == capacity) ++producerWaits;
        changed.wait(lock, [&] { return closed || cancelled || data.size() < capacity; });
        if (closed || cancelled) return false;
        data.push(value);
        peak = std::max(peak, data.size());
        lock.unlock();
        changed.notify_all();
        return true;
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!closed && !cancelled && data.empty()) ++consumerWaits;
        changed.wait(lock, [&] { return closed || cancelled || !data.empty(); });
        if (cancelled || data.empty()) return false;
        value = data.front();
        data.pop();
        lock.unlock();
        changed.notify_all();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
        changed.notify_all();
    }

    void cancel() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancelled = true;
        }
        changed.notify_all();
    }

    void printStats(const char* name) {
        std::lock_guard<std::mutex> lock(mutex);
        std::cout << name << ": максимум элементов " << peak << '/' << capacity
                  << ", ожиданий данных " << consumerWaits
                  << ", ожиданий свободного места " << producerWaits << '\n';
    }
};

Number parseNumber(const std::string& text) {
    Number value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("Ожидалось целое неотрицательное число: " + text);
    return value;
}

Number gcd(Number a, Number b) {
    while (b != 0) {
        const Number remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

void delay(int milliseconds) {
    if (milliseconds > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// Только первый рабочий поток читает файл.
void readTasks(std::ifstream& input, BlockingQueue<Task>& tasks,
               int delayMs, std::size_t& count) {
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (lineNumber == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3); // Допускаем UTF-8 BOM в файле из Windows.
        std::istringstream row(line);
        std::string first, second, extra;
        if (!(row >> first)) continue;
        try {
            if (!(row >> second) || (row >> extra))
                throw std::invalid_argument("В строке должна быть ровно одна пара чисел.");
            const Task task{parseNumber(first), parseNumber(second)};
            if (task.a == 0 && task.b == 0)
                throw std::invalid_argument("Пара (0, 0) не допускается.");
            delay(delayMs);
            if (!tasks.push(task)) return;
            ++count;
        }
        catch (const std::exception& error) {
            throw std::runtime_error("Строка " + std::to_string(lineNumber) + ": " + error.what());
        }
    }
    if (input.bad() || (input.fail() && !input.eof()))
        throw std::runtime_error("Ошибка чтения входного файла.");
    input.clear(); // Снимаем обычный EOF перед проверкой закрытия.
    input.close();
    if (input.fail()) throw std::runtime_error("Ошибка закрытия входного файла.");
    tasks.close();
}

// Только второй рабочий поток вычисляет НОД.
void calculateResults(BlockingQueue<Task>& tasks, BlockingQueue<Result>& results,
                      int delayMs, std::size_t& count) {
    Task task{};
    while (tasks.pop(task)) {
        delay(delayMs);
        const Result result{task.a, task.b, gcd(task.a, task.b)};
        if (!results.push(result)) return;
        ++count;
    }
    results.close();
}

// Только третий рабочий поток записывает выходной файл.
void writeResults(std::ofstream& output, BlockingQueue<Result>& results,
                  int delayMs, std::size_t& count) {
    Result result{};
    while (results.pop(result)) {
        delay(delayMs);
        output << result.a << ' ' << result.b << ' ' << result.gcd << '\n';
        if (!output) throw std::runtime_error("Ошибка записи выходного файла.");
        ++count;
    }
    output.close();
    if (output.fail()) throw std::runtime_error("Ошибка закрытия выходного файла.");
}

int askInt(const char* prompt, int minimum, int maximum) {
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) throw std::runtime_error("Ввод прерван.");
    std::istringstream input(line);
    int value = 0;
    std::string extra;
    if (!(input >> value) || (input >> extra) || value < minimum || value > maximum)
        throw std::invalid_argument("Число должно быть в диапазоне "
            + std::to_string(minimum) + ".." + std::to_string(maximum) + '.');
    return value;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        std::cout << "ЛР 2. Конвейер вычисления НОД. Три рабочих потока.\n"
                  << "Рабочая папка: " << std::filesystem::current_path().string() << '\n'
                  << "Входной файл [Enter = input.txt]: ";
        std::string inputName;
        if (!std::getline(std::cin, inputName)) throw std::runtime_error("Ввод прерван.");
        if (inputName.empty()) inputName = "input.txt";
        std::cout << "0 — без задержек\n1 — медленное чтение\n"
                  << "2 — медленное вычисление\n3 — медленная запись\n";
        const int mode = askInt("Режим: ", 0, 3);
        const int delayMs = mode == 0 ? 0 : askInt("Задержка этапа, мс: ", 1, 60000);
        const int capacity = askInt("Ёмкость каждой очереди (например, 4): ", 1, 1000000);
        const char* names[] = {"output_normal.txt", "output_read.txt",
                               "output_compute.txt", "output_write.txt"};
        const std::filesystem::path inputPath = std::filesystem::absolute(inputName).lexically_normal();
        const std::filesystem::path outputPath = std::filesystem::absolute(names[mode]).lexically_normal();
        if (inputPath == outputPath || (std::filesystem::exists(outputPath)
            && std::filesystem::equivalent(inputPath, outputPath)))
            throw std::invalid_argument("Входной и выходной файлы должны различаться.");
        std::ifstream input(inputPath);
        if (!input) throw std::runtime_error("Не удалось открыть входной файл: " + inputName);
        std::ofstream output(outputPath);
        if (!output) throw std::runtime_error("Не удалось создать выходной файл.");
        BlockingQueue<Task> tasks(static_cast<std::size_t>(capacity));
        BlockingQueue<Result> results(static_cast<std::size_t>(capacity));
        std::size_t readCount = 0, computedCount = 0, writtenCount = 0;
        std::exception_ptr failure;
        std::mutex errorMutex;
        auto guarded = [&](auto action) {
            try { action(); }
            catch (...) {
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!failure) failure = std::current_exception();
                }
                tasks.cancel();
                results.cancel();
            }
        };
        std::thread reader, calculator, writer;
        const auto start = Clock::now();
        try {
            reader = std::thread([&] {
                guarded([&] { readTasks(input, tasks, mode == 1 ? delayMs : 0, readCount); });
            });
            calculator = std::thread([&] {
                guarded([&] { calculateResults(tasks, results, mode == 2 ? delayMs : 0, computedCount); });
            });
            writer = std::thread([&] {
                guarded([&] { writeResults(output, results, mode == 3 ? delayMs : 0, writtenCount); });
            });
        }
        catch (...) {
            tasks.cancel();
            results.cancel();
            if (reader.joinable()) reader.join();
            if (calculator.joinable()) calculator.join();
            if (writer.joinable()) writer.join();
            throw;
        }
        reader.join();
        calculator.join();
        writer.join();
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        if (failure) std::rethrow_exception(failure);
        if (readCount != computedCount || computedCount != writtenCount)
            throw std::runtime_error("Число прочитанных и обработанных задач различается.");
        std::cout << "\nПрочитано: " << readCount << "; вычислено: " << computedCount
                  << "; записано: " << writtenCount << '\n'
                  << "Задержки чтения / вычисления / записи: "
                  << (mode == 1 ? delayMs : 0) << " / " << (mode == 2 ? delayMs : 0)
                  << " / " << (mode == 3 ? delayMs : 0) << " мс на запись\n";
        tasks.printStats("Q1");
        results.printStats("Q2");
        std::cout << "Время конвейера: " << std::fixed << std::setprecision(6) << elapsed << " с\n"
                  << "Результаты: " << outputPath.string() << '\n'
                  << "Формат строки: a b НОД(a,b). Порядок исходных пар сохранён.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Ошибка: " << error.what() << "\nПри ошибке выходной файл может быть неполным.\n";
        return 1;
    }
}
