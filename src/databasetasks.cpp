// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "databasetasks.h"

#include "tasks.h"

extern Dispatcher g_dispatcher;

void DatabaseTasks::start()
{
	db.connect();
	ThreadHolder::start();
}

void DatabaseTasks::threadMain()
{
	while (getState() == THREAD_STATE_RUNNING){
		DatabaseTask task;
		{
			std::unique_lock uniqueLock(taskLock);
			if (tasks.empty()) {
				taskSignal.wait(uniqueLock);
			}

			if(!tasks.empty()) {
				task = std::move(tasks.front());
				tasks.pop_front();
			}
		}

		if(task){
			runTask(std::move(task));
		}
	}
}

void DatabaseTasks::addTask(std::string query, std::function<void(DBResult_ptr, bool)> callback /* = nullptr*/,
                            bool store /* = false*/)
{
	std::lock_guard lockGuard(taskLock);
	if (getState() != THREAD_STATE_TERMINATED) {
		if(tasks.empty()){
			taskSignal.notify_one();
		}
		tasks.emplace_back(std::move(query), std::move(callback), store);
	}
}

void DatabaseTasks::runTask(DatabaseTask &&task)
{
	if(!task){
		return;
	}

	bool success;
	DBResult_ptr result;
	if (task.store) {
		result = db.storeQuery(task.query);
		success = true;
	} else {
		result = nullptr;
		success = db.executeQuery(task.query);
	}

	if (task.callback) {
		g_dispatcher.addTask(
			[result, success, callback = std::move(task.callback)] {
				callback(result, success);
			});
	}
}

void DatabaseTasks::flush()
{
	while(true){
		DatabaseTask task;
		{
			std::lock_guard lockGuard(taskLock);
			if(!tasks.empty()) {
				task = std::move(tasks.front());
				tasks.pop_front();
			}
		}

		if(task){
			runTask(std::move(task));
		}else{
			break;
		}
	}
}

void DatabaseTasks::shutdown()
{
	{
		std::lock_guard lockGuard(taskLock);
		setState(THREAD_STATE_TERMINATED);
		taskSignal.notify_one();
	}

	flush();
}
