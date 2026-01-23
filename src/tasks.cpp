// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "tasks.h"

#include "enums.h"
#include "game.h"

extern Game g_game;

Task* createTask(TaskFunc&& f) { return new Task(std::move(f)); }

Task* createTask(uint32_t expiration, TaskFunc&& f) { return new Task(expiration, std::move(f)); }

void Dispatcher::threadMain()
{
	std::vector<Task*> tmpTaskList;
	while (getState() == THREAD_STATE_RUNNING) {
		{
			// check if there are tasks waiting
			std::unique_lock uniqueLock(taskLock);
			if (taskList.empty()) {
				// if the list is empty wait for signal
				taskSignal.wait(uniqueLock);
			}
			tmpTaskList.swap(taskList);
		}

		for (Task* task : tmpTaskList) {
			if (!task->hasExpired()) {
				++dispatcherCycle;
				// execute it
				(*task)();
			}
			delete task;
		}
		tmpTaskList.clear();
	}
}

void Dispatcher::addTask(Task* task)
{
	std::lock_guard lockGuard(taskLock);
	if(getState() != THREAD_STATE_TERMINATED){
		if(taskList.empty()){
			taskSignal.notify_one();
		}
		taskList.push_back(task);
	} else {
		delete task;
	}
}

void Dispatcher::shutdown()
{
	addTask([this] { setState(THREAD_STATE_TERMINATED); });
}
