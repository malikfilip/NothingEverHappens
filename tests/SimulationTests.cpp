#include "simulator/Simulation.hpp"

#include <functional>
#include <iostream>
#include <memory>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace simulator;

namespace {
void check(bool condition, const std::source_location location = std::source_location::current())
{
    if (!condition) throw std::runtime_error("Simulation check failed at line " + std::to_string(location.line()));
}

class ActionEvent : public Event {
public:
    ActionEvent(double time, std::function<void()> action)
        : Event(time), action_(std::move(action)) {}
    void execute() override { action_(); }
private:
    std::function<void()> action_;
};

void emptyAndSingleEvent()
{
    Simulation simulation;
    int executions = 0;
    int observations = 0;
    simulation.setEventObserver([&](const Event&) { ++observations; });
    check(!simulation.nextEventTime());
    check(!simulation.step());
    check(simulation.currentTime() == 0 && observations == 0);
    simulation.schedule(std::make_unique<ActionEvent>(4, [&] {
        check(simulation.currentTime() == 4);
        ++executions;
    }));
    check(simulation.nextEventTime() == 4);
    check(simulation.currentTime() == 0 && observations == 0);
    check(simulation.step());
    check(!simulation.nextEventTime());
    check(executions == 1 && observations == 1 && simulation.currentTime() == 4);
    check(!simulation.step());
    check(executions == 1 && observations == 1 && simulation.currentTime() == 4);
}

void orderingAndRun()
{
    // Exercise the same scenario with stepping and with run(), including an
    // event scheduled at the current time by another event.
    for (const bool useRun : {false, true}) {
        Simulation simulation;
        std::vector<int> executed;
        std::vector<double> times;
        std::vector<std::uint64_t> sequences;
        simulation.setEventObserver([&](const Event& event) {
            check(simulation.currentTime() == event.time());
            check(executed.size() == times.size()); // Observer precedes execution.
            times.push_back(event.time());
            sequences.push_back(event.sequence());
        });
        auto schedule = [&](double time, int id) {
            simulation.schedule(std::make_unique<ActionEvent>(time, [&, id] {
                executed.push_back(id);
                if (id == 1) {
                    simulation.schedule(std::make_unique<ActionEvent>(2, [&] { executed.push_back(4); }));
                }
            }));
        };
        schedule(7, 0);
        schedule(2, 1);
        schedule(2, 2);
        schedule(5, 3);
        const std::vector<int> expectedIds{1, 2, 4, 3, 0};
        const std::vector<double> expectedTimes{2, 2, 2, 5, 7};
        if (useRun) {
            simulation.run();
        } else {
            for (std::size_t i = 0; i < expectedIds.size(); ++i) {
                check(simulation.step());
                check(executed.size() == i + 1);
                check(executed.back() == expectedIds[i]);
                check(simulation.currentTime() == expectedTimes[i]);
            }
        }
        check(executed == expectedIds);
        check(times == expectedTimes);
        check(sequences == std::vector<std::uint64_t>({1, 2, 4, 3, 0}));
        check(!simulation.step() && simulation.currentTime() == 7);
        simulation.run();
        check(executed == expectedIds && simulation.currentTime() == 7);
    }
}

void immediateEventsAndTracing()
{
    Simulation simulation(true);
    std::ostringstream output;
    struct Capture {
        std::streambuf* previous;
        explicit Capture(std::ostringstream& stream) : previous(std::cout.rdbuf(stream.rdbuf())) {}
        ~Capture() { std::cout.rdbuf(previous); }
    } capture(output);
    std::vector<int> order;
    std::vector<std::uint64_t> sequences;
    simulation.setEventObserver([&](const Event& event) {
        // Tracing happens before observation.
        const auto count = sequences.size() + 1;
        std::string expected;
        for (std::size_t i = 0; i < count; ++i) expected += "[t=3.000000] EVENT\n";
        check(output.str() == expected);
        check(event.time() == 3 && simulation.currentTime() == 3);
        sequences.push_back(event.sequence());
        order.push_back(0);
    });
    simulation.schedule(std::make_unique<ActionEvent>(3, [&] {
        order.push_back(1);
        ActionEvent immediate(3, [&] { order.push_back(2); });
        simulation.executeNow(immediate);
        order.push_back(3);
    }));
    simulation.schedule(std::make_unique<ActionEvent>(3, [&] { order.push_back(4); }));
    check(simulation.step());
    check(order == std::vector<int>({0, 1, 0, 2, 3}));
    check(sequences == std::vector<std::uint64_t>({0, 2}));
    check(simulation.step());
    check(order == std::vector<int>({0, 1, 0, 2, 3, 0, 4}));
    check(sequences == std::vector<std::uint64_t>({0, 2, 1}));
    check(!simulation.step());
}

void exceptionPropagation()
{
    for (const bool observerThrows : {false, true}) {
        Simulation simulation;
        bool executed = false;
        bool laterExecuted = false;
        simulation.setEventObserver([&](const Event&) {
            if (observerThrows) throw std::runtime_error("observer");
        });
        simulation.schedule(std::make_unique<ActionEvent>(2, [&] {
            executed = true;
            throw std::runtime_error("event");
        }));
        simulation.schedule(std::make_unique<ActionEvent>(5, [&] { laterExecuted = true; }));
        bool caught = false;
        try { simulation.step(); }
        catch (const std::runtime_error& error) {
            caught = true;
            check(std::string(error.what()) == (observerThrows ? "observer" : "event"));
        }
        check(caught && executed == !observerThrows);
        check(simulation.currentTime() == 2 && !laterExecuted);
        simulation.setEventObserver({});
        check(simulation.step()); // Failed event was removed; remaining queue is intact.
        check(laterExecuted && simulation.currentTime() == 5);
        check(!simulation.step());
    }
}
}

int main()
{
    try {
        emptyAndSingleEvent();
        orderingAndRun();
        immediateEventsAndTracing();
        exceptionPropagation();
        std::cout << "Simulation tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
