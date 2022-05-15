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

#define NO_IO -1

// Functie ajutatoare din laboratoare
#define DIE(assertion, call_description)  \
	do                                    \
	{                                     \
		if (assertion)                    \
		{                                 \
			fprintf(stderr, "(%s, %d): ", \
					__FILE__, __LINE__);  \
			perror(call_description);     \
			exit(EXIT_FAILURE);           \
		}                                 \
	} while (0)

// Structura pnetru a defini un thread
typedef struct {
	so_handler *handler;
	int priority;
	int status;
	int runningTime;
	tid_t id;
	sem_t alarm;
	unsigned int io;
} my_thread;

// Structura pentru a defini Scheduler-ul
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

// Adauga inca un thread in lista de threaduri
void updateList(my_thread *thr)
{
	scheduler.threads[scheduler.thrNo++] = thr;
}

// Adauga un thread in coada. Coada contine elementele sortate
// descrescator.
// Seteaza statusul ca READY.
void updateQueue(my_thread *thr)
{
	unsigned int i = 0, j;

	for (; i < scheduler.qSize; ++i)
		if (thr->priority > scheduler.queue[i]->priority)
			break;

	for (j = scheduler.qSize; j > i; j--)
		scheduler.queue[j] = scheduler.queue[j - 1];

	thr->status = READY;
	scheduler.qSize++;
	scheduler.queue[i] = thr;
}

// Sterg primul element din coada si devine thread-ul curent
// care va rula urmatoarea comanda.
void removeTop(void)
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

// Alege un thread-ul care va rula in continuare.
// Daca Thread-ul curent e WAITING sau TERMINATED
// atunci il schimb si nu il bag in coada.
int chooseThread(void)
{
	my_thread *thr = scheduler.queue[0];
	my_thread *current = scheduler.currentThread;

	if (current->status == WAITING ||
		current->status == TERMINATED)
	{
		removeTop();
		return 1;
	}

	// Daca este un thread cu o prioritate mai mare
	// sau cu aceeasi prioritate, iar thread-ul curent
	// si-a terminat cuanta de timp, atunci il bag in coada
	// si modific thread-ul care va rula.
	if (current->priority < thr->priority ||
		(current->priority == thr->priority &&
		 current->runningTime <= 0))
	{
		removeTop();
		updateQueue(current);

		return 1;
	}

	return 0;
}

// Ruleaza thread-ul curent.
// Scoate thread-ul din starea d ewaiting a semaforului.
void runCurrentThread(int time)
{
	my_thread *thr = scheduler.currentThread;

	thr->runningTime = time;
	thr->status = RUNNING;

	int rc = sem_post(&thr->alarm);

	DIE(rc < 0, "Error on waking up thread");
}

// Cauta urmatorul thread care va rula.
void schedule(void)
{
	my_thread *previousThread = scheduler.currentThread;

	if (scheduler.currentThread == NULL) // Prima rulare
		removeTop();
	else if ((previousThread && scheduler.qSize == 0) || !chooseThread())
	{
		// Daca nu avem niciun thread, rulam acelasi thread.
		// Daca timpul a expirat, il resetez.
		if (previousThread->runningTime <= 0)
		{
			previousThread->runningTime = scheduler.time;
		}

		runCurrentThread(previousThread->runningTime);
		return;
	}

	// Rulez thread-ul curent.
	runCurrentThread(scheduler.time);
}

// Functia care ruleaza la pornirea thread-ului.
// Trec thread-ul in waiting, folosind semaforul.
// Rulez handler-ul si setez statusul ca TERMINATED.
void *startThread(void *args)
{
	my_thread *thr = (my_thread *)args;

	int rc = sem_wait(&thr->alarm);

	DIE(rc < 0, "Error on waiting semaphore");

	thr->handler(thr->priority);
	thr->status = TERMINATED;

	schedule();

	return NULL;
}

// Initializez scheduler-ul.
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
	scheduler.threads = (my_thread **)malloc(sizeof(my_thread *) * THREADS_NO);
	scheduler.queue = (my_thread **)malloc(sizeof(my_thread *) * THREADS_NO);

	return SUCCESS;
}

// Creez un nou thread, il bag in lista si in coada.
// Daca nu ruleaza niciun thread, atunci rulez trebuie sa caut
// unul. Daca ruleaza un thread, atunci rulez so_exec().
tid_t so_fork(so_handler *func, unsigned int priority)
{
	if (priority > SO_MAX_PRIO || !func)
		return INVALID_TID;

	my_thread *thr = (my_thread *)malloc(sizeof(my_thread));

	thr->priority = priority;
	thr->handler = func;
	thr->status = NEW;
	thr->id = INVALID_TID;
	thr->runningTime = scheduler.time;
	thr->io = NO_IO;

	int rc = sem_init(&thr->alarm, 0, 0);

	DIE(rc < 0, "Error on creating semaphore");
	rc = pthread_create(&thr->id, NULL, (void *)startThread, (void *)thr);
	DIE(rc < 0, "Error on creating new thread");

	updateQueue(thr);
	updateList(thr);

	if (!scheduler.currentThread)
		schedule();
	else
		so_exec();

	return thr->id;
}

// Trec un thread in starea de WAITING.
// Setez semnalul IO pe care trebuie sa il primeasca ca fiind
// argumentul functiei.
int so_wait(unsigned int io)
{
	if (io >= scheduler.io)
		return ERROR;

	my_thread *thr = scheduler.currentThread;

	thr->status = WAITING;
	thr->io = io;
	schedule();

	int rc = sem_wait(&thr->alarm);

	DIE(rc < 0, "Error on putting thread in wait");

	return SUCCESS;
}

// Parcurg lista cu toate thread-urile si le scot din starea WAITING.
// Pun thread-ul curent in starea waiting a semaforului, deoarece
// altfel acesta se va pierde.
int so_signal(unsigned int io)
{
	if (io >= scheduler.io)
		return ERROR;

	int nr = 0;
	unsigned int i = 0;

	for (; i < scheduler.thrNo; ++i)
	{
		my_thread *thr = scheduler.threads[i];

		if (thr->status == WAITING && thr->io == io)
		{
			thr->io = NO_IO;
			updateQueue(thr);
			nr++;
		}
	}

	schedule();

	int rc = sem_wait(&scheduler.currentThread->alarm);

	DIE(rc < 0, "Error on putting thread in wait");

	return nr;
}

// Scad cuanta d etimp si fac schedule() pentru a vedea daca este un alt
// thread favorit.
void so_exec(void)
{
	my_thread *runningThread = scheduler.currentThread;

	runningThread->runningTime--;
	schedule();

	int rc = sem_wait(&runningThread->alarm);

	DIE(rc < 0, "Error on puting thread in waiting state");
}

// Distrug toate semafoarele si thread-urile. Eliberez memoria.
void so_end(void)
{
	if (scheduler.init == 0)
		return;

	scheduler.init = 0;
	unsigned int i = 0;
	int rc;

	for (i = 0; i < scheduler.thrNo; ++i)
	{
		rc = pthread_join(scheduler.threads[i]->id, NULL);
		DIE(rc < 0, "Can't join threads");

		rc = sem_destroy(&scheduler.threads[i]->alarm);
		DIE(rc < 0, "Can't destroy semaphore");

		free(scheduler.threads[i]);
	}

	free(scheduler.threads);
	free(scheduler.queue);
}
