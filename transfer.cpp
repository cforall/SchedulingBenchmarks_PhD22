#include "rq_bench.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
	#include <libfibre/fibre.h>
#pragma GCC diagnostic pop

#define PRINT(...)

__lehmer64_state_t lead_seed;
volatile unsigned leader;
volatile size_t lead_idx;

bool exhaust = false;
volatile bool estop = false;

bench_sem the_main;

class __attribute__((aligned(128))) MyThread;

MyThread ** threads;

class __attribute__((aligned(128))) MyThread {
	unsigned id;
	volatile size_t idx;
	bench_sem sem;

public:
	size_t rechecks;

	MyThread(unsigned _id)
		: id(_id), idx(0), rechecks(0)
	{}

	void unpark() { sem.post(); }
	void park  () { sem.wait(); }

	void waitgroup() {
		uint64_t start = timeHiRes();
		for(size_t i = 0; i < nthreads; i++) {
			PRINT( std::cout << "Waiting for : " << i << " (" << threads[i]->idx << ")" << std::endl; )
			while( threads[i]->idx != lead_idx ) {
				Pause();
				if( to_miliseconds(timeHiRes() - start) > 5'000 ) {
					std::cerr << "Programs has been blocked for more than 5 secs" << std::endl;
					estop = true;
					the_main.post();
					goto END;
				}
			}
		}
		END:;
		PRINT( std::cout | "Waiting done"; )
	}

	void wakegroup(unsigned me) {
		if(!exhaust) return;

		for(size_t i = 0; i < nthreads; i++) {
			if(i!= me) threads[i]->sem.post();
		}
	}

	void lead() {
		this->idx = ++lead_idx;
		if(lead_idx > stop_count || estop) {
			PRINT( std::cout << "Leader " << this->id << " done" << std::endl; )
			the_main.post();
			return;
		}

		PRINT( sout << "Leader no " << this->idx << ": " << this->id << std::endl; )

		waitgroup();

		unsigned nleader = __lehmer64( lead_seed ) % nthreads;
		__atomic_store_n( &leader, nleader, __ATOMIC_SEQ_CST );

		wakegroup(this->id);
	}

	void wait() {
		fibre_yield();
		if(lead_idx == this->idx) {
			this->rechecks++;
			return;
		}

		assert( (lead_idx - 1) == this->idx );
		__atomic_add_fetch( &this->idx, 1, __ATOMIC_SEQ_CST );
		if(exhaust) this->sem.wait();
		else fibre_yield();
	}

	static void main(MyThread * arg) {
		MyThread & self = *arg;
		self.park();

		unsigned me = self.id;

		for(;;) {
			if(leader == me) {
				self.lead();
			}
			else {
				self.wait();
			}
			if(lead_idx > stop_count || estop) break;
		}
	}
};

// ==================================================
int main(int argc, char * argv[]) {
	__lehmer64_state_t lead_seed = getpid();
	for(int i = 0; i < 10; i++) __lehmer64( lead_seed );
	unsigned nprocs = 2;

	option_t opt[] = {
		BENCH_OPT,
		{ 'e', "exhaust", "Whether or not threads that have seen the new epoch should yield or park.", exhaust, parse_yesno}
	};
	BENCH_OPT_PARSE("cforall transition benchmark");

	std::cout.imbue(std::locale(""));
	setlocale(LC_ALL, "");

	if(clock_mode) {
		std::cerr << "This benchmark doesn't support duration mode" << std::endl;
		return 1;
	}

	if(nprocs < 2) {
		std::cerr << "Must have at least 2 processors" << std::endl;
		return 1;
	}

	lead_idx = 0;
	leader = __lehmer64( lead_seed ) % nthreads;

	size_t rechecks = 0;

	uint64_t start, end;
	{
		FibreInit(1, nprocs);
		{
			Fibre ** handles = new Fibre*[nthreads];
			threads = new MyThread*[nthreads];
			for(size_t i = 0; i < nthreads; i++) {
				threads[i] = new MyThread( i );
				handles[i] = new Fibre();
				handles[i]->run( MyThread::main, threads[i] );
			}

			start = timeHiRes();
			for(size_t i = 0; i < nthreads; i++) {
				threads[i]->unpark();
			}

			the_main.wait();
			end = timeHiRes();

			for(size_t i = 0; i < nthreads; i++) {
				threads[i]->unpark();
			}

			for(size_t i = 0; i < nthreads; i++) {
				MyThread & thrd = *threads[i];
				fibre_join( handles[i], nullptr );
				PRINT( std::cout << i << " joined" << std::endl; )
				rechecks += thrd.rechecks;
				delete( threads[i] );
			}

			delete[] (threads);
			delete[] (handles);
		}
	}

	std::cout << "Duration (ms)           : " << to_miliseconds(end - start) << std::endl;
	std::cout << "Number of processors    : " << nprocs << std::endl;
	std::cout << "Number of threads       : " << nthreads << std::endl;
	std::cout << "Total Operations(ops)   : " << (lead_idx - 1) << std::endl;
	std::cout << "Threads parking on wait : " << (exhaust ? "yes" : "no") << std::endl;
	std::cout << "Rechecking              : " << rechecks << std::endl;
	std::cout << "ns per transfer         : " << std::fixed << (((double)(end - start)) / (lead_idx)) << std::endl;


}