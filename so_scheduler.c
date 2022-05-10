#include "util/so_scheduler.h"
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <semaphore.h>

#define NEW 0
#define READY 1
#define RUNNING 2
#define WAITING 3
#define TERMINATED 4

#define ERROR -1
#define SUCCESS 0
#define THREADS_NO 9999

#define DIE(assertion, call_description)				\
	do {												\
		if (assertion) {								\
			fprintf(stderr, "(%s, %d): ",				\
					__FILE__, __LINE__);				\
			perror(call_description);					\
			exit(EXIT_FAILURE);							\
		}												\
	} while (0)

typedef struct {
	so_handler *handler;
	int priority;
	int status;
	int runningTime;
	tid_t id;
	sem_t alarm;
} my_thread;

typedef struct {
	unsigned int time;
	unsigned int io;
	unsigned short init;
	unsigned int thrNo;
	unsigned int qSize;

	my_thread *currentThread;
	my_thread **threads;
	my_thread **queue;
} so_scheduler;

static so_scheduler scheduler;

void updateList(my_thread *thr)
{
	scheduler.threads[scheduler.thrNo++] = thr;
}

void updateQueue(my_thread *thr)
{
	unsigned int i = 0, j;

	for (; i < scheduler.qSize; ++i)
		if (thr->priority > scheduler.queue[i]->priority)
			break;
		
	
	for (j = scheduler.qSize; j > i; j--)
		scheduler.queue[j] = scheduler.queue[j - 1];

	scheduler.qSize++;
	scheduler.queue[i] = thr;
	scheduler.queue[i]->status = READY;
}

void removeTop()
{
	if (scheduler.qSize < 1)
		return;

	scheduler.currentThread = scheduler.queue[0];
	scheduler.qSize--;

	unsigned int i = 0;

	for (; i < scheduler.qSize; ++i)
		scheduler.queue[i] = scheduler.queue[i + 1];

	scheduler.queue[scheduler.qSize] = NULL;
}

int chooseThread()
{
	my_thread *thr = scheduler.queue[0];
	my_thread *current = scheduler.currentThread;

	if (current->status == WAITING ||
		current->status == TERMINATED) {
			removeTop();
			return 1;
		}

	if (current->priority < thr->priority ||
		(current->priority == thr->priority &&
		current->runningTime <= 0)) {
		removeTop();
		updateQueue(current);
		
		return 1;
	}
	
	return 0;
}

void runCurrentThread() {
	my_thread *thr = scheduler.currentThread;

	thr->runningTime = scheduler.time;
	thr->status = RUNNING;

	int rc = sem_post(&thr->alarm);
	DIE(rc < 0, "Error on waking up thread");
}

void schedule()
{
	my_thread *previousThread = scheduler.currentThread;

	if (scheduler.currentThread == NULL)
		removeTop();
	else if ((previousThread && scheduler.qSize == 0 ) || !chooseThread()) {
		if (previousThread->runningTime <= 0) {
			previousThread->runningTime = scheduler.time;
		}
	}

	runCurrentThread();
}

void startThread(void *args)
{
	my_thread *thr = (my_thread *) args;

	int rc = sem_wait(&thr->alarm);
	DIE(rc < 0, "Error on waiting semaphore");

	thr->handler(thr->priority);
	thr->status = TERMINATED;

	schedule();
}

int so_init(unsigned int time_quantum, unsigned int io)
{
	if (io > SO_MAX_NUM_EVENTS || time_quantum == 0 || scheduler.init == 1)
		return ERROR;

	scheduler.time = time_quantum;
	scheduler.io = io;
	scheduler.init = 1;
	scheduler.thrNo = 0;
	scheduler.qSize = 0;
	scheduler.currentThread = NULL;
	scheduler.threads = (my_thread **) malloc(sizeof(my_thread *) * THREADS_NO);
	scheduler.queue = (my_thread **) malloc(sizeof(my_thread *) * THREADS_NO);

	return SUCCESS;
}

tid_t so_fork(so_handler *func, unsigned int priority)
{
	if (priority > SO_MAX_PRIO || !func)
		return INVALID_TID;

	my_thread *thr = (my_thread *) malloc(sizeof(my_thread));

	thr->priority = priority;
	thr->handler = func;
	thr->status = NEW;
	thr->id = INVALID_TID;
	thr->runningTime = scheduler.time;

	int rc = sem_init(&thr->alarm, 0, 0);
	DIE(rc < 0, "Error on creating semaphore");

	rc = pthread_create(&thr->id, NULL, (void *) startThread, (void *) thr);
	DIE(rc < 0, "Error on creating new thread");

	updateQueue(thr);
	updateList(thr);

	if (!scheduler.currentThread)
		schedule();
	else
		so_exec();

	return thr->id;
}

int so_wait(unsigned int io)
{
	if (io >= scheduler.io)
		return ERROR;

	scheduler.currentThread->status = WAITING;
	schedule();

	int rc = sem_wait(&scheduler.currentThread->alarm);
	DIE(rc < 0, "Error on putting thread in wait");

	return SUCCESS;
}

int so_signal(unsigned int io)
{
	return io >= scheduler.io ? ERROR : 0;
}

void so_exec(void)
{
	my_thread *runningThread = scheduler.currentThread;

	runningThread->runningTime--;
	schedule();

	int rc = sem_wait(&runningThread->alarm);
	DIE(rc < 0, "Error on puting thread in waiting state");
}

void so_end(void)
{
	if (scheduler.init == 0)
		return;

	scheduler.init = 0;
	unsigned int i = 0;
	int rc;

	for (i = 0; i < scheduler.thrNo; ++i) {
		rc = pthread_join(scheduler.threads[i]->id, NULL);
		DIE(rc < 0, "Can't join threads");

		rc = sem_destroy(&scheduler.threads[i]->alarm);
		DIE(rc < 0, "Can't destroy semaphore");

		free(scheduler.threads[i]);
	}

	free(scheduler.threads);
	free(scheduler.queue);
}
