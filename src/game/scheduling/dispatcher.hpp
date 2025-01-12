/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

#include "task.hpp"
#include "lib/thread/thread_pool.hpp"

static constexpr uint16_t DISPATCHER_TASK_EXPIRATION = 2000;
static constexpr uint16_t SCHEDULER_MINTICKS = 50;

enum class TaskGroup : int8_t {
	ThreadPool = -1,
	Serial,
	GenericParallel,
	Last
};

enum class DispatcherType : uint8_t {
	None,
	Event,
	AsyncEvent,
	ScheduledEvent,
	CycleEvent
};

struct DispatcherContext {
	bool isOn() const {
		return Task::TIME_NOW != SYSTEM_TIME_ZERO;
	}

	bool isGroup(const TaskGroup _group) const {
		return group == _group;
	}

	bool isAsync() const {
		return group != TaskGroup::Serial;
	}

	auto getGroup() const {
		return group;
	}

	auto getName() const {
		return taskName;
	}

	auto getType() const {
		return type;
	}

private:
	void reset() {
		group = TaskGroup::ThreadPool;
		type = DispatcherType::None;
		taskName = "ThreadPool::call";
	}

	DispatcherType type = DispatcherType::None;
	TaskGroup group = TaskGroup::ThreadPool;
	std::string_view taskName = "";

	friend class Dispatcher;
};

/**
 * Dispatcher allow you to dispatch a task async to be executed
 * in the dispatching thread. You can dispatch with an expiration
 * time, after which the task will be ignored.
 */
class Dispatcher {
public:
	explicit Dispatcher(ThreadPool &threadPool) :
		threadPool(threadPool) {
		threads.reserve(std::thread::hardware_concurrency() + 1);
		for (uint_fast16_t i = 0; i < std::thread::hardware_concurrency() + 1; ++i) {
			threads.emplace_back(std::make_unique<ThreadTask>());
		}
	};

	// Ensures that we don't accidentally copy it
	Dispatcher(const Dispatcher &) = delete;
	Dispatcher operator=(const Dispatcher &) = delete;

	static Dispatcher &getInstance();

	void addEvent(std::function<void(void)> &&f, std::string_view context, uint32_t expiresAfterMs = 0);

	uint64_t cycleEvent(uint32_t delay, std::function<void(void)> &&f, std::string_view context) {
		return scheduleEvent(delay, std::move(f), context, true);
	}

	uint64_t scheduleEvent(const std::shared_ptr<Task> &task);
	uint64_t scheduleEvent(uint32_t delay, std::function<void(void)> &&f, std::string_view context) {
		return scheduleEvent(delay, std::move(f), context, false);
	}

	void asyncEvent(std::function<void(void)> &&f, TaskGroup group = TaskGroup::GenericParallel);

	uint64_t asyncCycleEvent(uint32_t delay, std::function<void(void)> &&f, TaskGroup group = TaskGroup::GenericParallel) {
		return scheduleEvent(
			delay, [this, f = std::move(f), group] { asyncEvent([f] { f(); }, group); }, dispacherContext.taskName, true, false
		);
	}

	uint64_t asyncScheduleEvent(uint32_t delay, std::function<void(void)> &&f, TaskGroup group = TaskGroup::GenericParallel) {
		return scheduleEvent(
			delay, [this, f = std::move(f), group] { asyncEvent([f] { f(); }, group); }, dispacherContext.taskName, false, false
		);
	}

	[[nodiscard]] uint64_t getDispatcherCycle() const {
		return dispatcherCycle;
	}

	void stopEvent(uint64_t eventId);

	const auto &context() const {
		return dispacherContext;
	}

private:
	thread_local static DispatcherContext dispacherContext;

	// Update Time Cache
	static void updateClock() {
		Task::TIME_NOW = std::chrono::system_clock::now();
	}

	static int16_t getThreadId() {
		static std::atomic_int16_t lastId = -1;
		thread_local static int16_t id = -1;

		if (id == -1) {
			lastId.fetch_add(1);
			id = lastId.load();
		}

		return id;
	};

	uint64_t scheduleEvent(uint32_t delay, std::function<void(void)> &&f, std::string_view context, bool cycle, bool log = true) {
		return scheduleEvent(std::make_shared<Task>(std::move(f), context, delay, cycle, log));
	}

	void init();
	void shutdown() {
		signalAsync.notify_all();
	}

	inline void mergeEvents();
	inline void executeEvents(std::unique_lock<std::mutex> &asyncLock);
	inline void executeScheduledEvents();

	inline void executeSerialEvents(std::vector<Task> &tasks);
	inline void executeParallelEvents(std::vector<Task> &tasks, const uint8_t groupId, std::unique_lock<std::mutex> &asyncLock);
	inline std::chrono::nanoseconds timeUntilNextScheduledTask() const;

	inline void checkPendingTasks() {
		hasPendingTasks = false;
		for (uint_fast8_t i = 0; i < static_cast<uint8_t>(TaskGroup::Last); ++i) {
			if (!m_tasks[i].empty()) {
				hasPendingTasks = true;
				break;
			}
		}
	}

	void notify() {
		if (!hasPendingTasks) {
			hasPendingTasks = true;
			signalSchedule.notify_one();
		}
	}

	uint_fast64_t dispatcherCycle = 0;

	ThreadPool &threadPool;
	std::condition_variable signalAsync;
	std::condition_variable signalSchedule;
	std::atomic_bool hasPendingTasks = false;
	std::mutex dummyMutex; // This is only used for signaling the condition variable and not as an actual lock.

	// Thread Events
	struct ThreadTask {
		ThreadTask() {
			for (auto &task : tasks) {
				task.reserve(2000);
			}
			scheduledTasks.reserve(2000);
		}

		std::array<std::vector<Task>, static_cast<uint8_t>(TaskGroup::Last)> tasks;
		std::vector<std::shared_ptr<Task>> scheduledTasks;
		std::mutex mutex;
	};
	std::vector<std::unique_ptr<ThreadTask>> threads;

	// Main Events
	std::array<std::vector<Task>, static_cast<uint8_t>(TaskGroup::Last)> m_tasks;
	std::priority_queue<std::shared_ptr<Task>, std::deque<std::shared_ptr<Task>>, Task::Compare> scheduledTasks;
	phmap::parallel_flat_hash_map_m<uint64_t, std::shared_ptr<Task>> scheduledTasksRef;

	friend class CanaryServer;
};

constexpr auto g_dispatcher = Dispatcher::getInstance;
