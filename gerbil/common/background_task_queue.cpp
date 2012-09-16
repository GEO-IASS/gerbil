/*	
	Copyright(c) 2012 Petr Koupy <petr.koupy@gmail.com>

	This file may be licensed under the terms of of the GNU General Public
	License, version 3, as published by the Free Software Foundation. You can
	find it here: http://www.gnu.org/licenses/gpl.html
*/

#include "background_task_queue.h"
#include <iostream>

void BackgroundTaskQueue::halt() 
{
	Lock lock(guard);
	halted = true;
	taskQueue.clear(); // Flush the queue, so there is nothing else to pop.
	if (currentTask) {
		cancelled = true;
		currentTask->cancel();
	}
	lock.unlock(); // Unlock to prevent deadlock when signalling the condition.
	future.notify_all(); // In case the thread is sleeping.
}

bool BackgroundTaskQueue::pop() 
{
	Lock lock(guard);
	while (taskQueue.empty()) {
		if (halted) {
			return false; // Thread will terminate.
		} else {
			future.wait(lock); // Yields lock until signalled.
		}
	}
	currentTask = taskQueue.front(); // Fetch the task.
	taskQueue.pop_front();
	cancelled = false;
	return true;
}

void BackgroundTaskQueue::push(BackgroundTaskPtr &task) 
{
	Lock lock(guard);
	taskQueue.push_back(task);
	lock.unlock(); // Unlock to prevent deadlock when signalling the condition.
	future.notify_all();
}

void BackgroundTaskQueue::cancelTasks(cv::Rect &roi) 
{
	Lock lock(guard);
	std::deque<BackgroundTaskPtr>::iterator it = taskQueue.begin();
	while (it != taskQueue.end()) {
		if ((*it)->roi() == roi) {
			it = taskQueue.erase(it);
		} else {
			++it;
		}
	}
	if (currentTask->roi() == roi) {
		cancelled = true;
		currentTask->cancel();
	}
}

void BackgroundTaskQueue::operator()() 
{
	std::cout << "BackgroundTaskQueue started." << std::endl;
	while (true) {
		if (!pop()) {
			break; // Thread termination.
		}
		bool success = currentTask->run();
		{
			Lock lock(guard);
			currentTask->done(!cancelled && success);
			currentTask.reset();
		}
	}
	std::cout << "BackgroundTaskQueue terminated." << std::endl;
}

BackgroundTaskQueue BackgroundTaskQueue::self;

BackgroundTaskQueue &BackgroundTaskQueue::instance() 
{
	return self;
}
