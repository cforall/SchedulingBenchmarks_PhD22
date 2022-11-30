#include "rq_bench.hpp"
#include <libfibre/fibre.h>

unsigned spot_cnt = 2;
FredSemaphore * spots;

struct Churner {
	unsigned long long count = 0;
	unsigned long long blocks = 0;
	bool skip = false;
	__lehmer64_state_t seed;
	bench_sem self;
};

void churner_main( Churner * self ) {
	fibre_park();
	for(;;) {
		unsigned r = __lehmer64( self->seed );
		FredSemaphore & sem = spots[r % spot_cnt];
		if(!self->skip) sem.V();
		self->blocks += sem.P() == SemaphoreWasOpen ? 1 : 0;
		self->skip = false;

		self->count ++;
		if( clock_mode && stop) break;
		if(!clock_mode && self->count >= stop_count) break;
	}

	__atomic_fetch_add(&threads_left, -1, __ATOMIC_SEQ_CST);
}

int main(int argc, char * argv[]) {
	option_t opt[] = {
		BENCH_OPT,
		{ 's', "spots", "Number of spots in the system", spot_cnt }
	};
	BENCH_OPT_PARSE("libfibre churn benchmark");

	{
		unsigned long long global_counter = 0;
		unsigned long long global_blocks  = 0;
		uint64_t start, end;
		FibreInit(1, nprocs );
		{
			spots = new FredSemaphore[spot_cnt]();

			threads_left = nthreads;
			Churner * thddata = new Churner[nthreads]();
			for(unsigned i = 0; i < nthreads; i++ ) {
				Churner & t = thddata[i];
				t.skip = i < spot_cnt;
				t.seed = rand();
			}
			Fibre * threads[nthreads];
			for(unsigned i = 0; i < nthreads; i++) {
				threads[i] = new Fibre();
				threads[i]->run(churner_main, &thddata[i]);
			}
			printf("Starting\n");

			bool is_tty = isatty(STDOUT_FILENO);
			start = timeHiRes();

			for(unsigned i = 0; i < nthreads; i++ ) {
				fibre_unpark( threads[i] );
			}
			wait<Fibre>(start, is_tty);

			stop = true;
			end = timeHiRes();
			printf("\nDone\n");

			for(unsigned i = 0; i < spot_cnt; i++) {
				for(int j = 0; j < 10000; j++) spots[i].V();
			}

			for(unsigned i = 0; i < nthreads; i++ ) {
				fibre_join( threads[i], nullptr );
				global_counter += thddata[i].count;
				global_blocks  += thddata[i].blocks;
			}

			delete[](spots);
			delete[](thddata);
		}

		printf("\nDone2\n");

		printf("Duration (ms)        : %'ld\n", to_miliseconds(end - start));
		printf("Number of processors : %'d\n", nprocs);
		printf("Number of threads    : %'d\n", nthreads);
		printf("Number of spots      : %'d\n", spot_cnt);
		printf("Total Operations(ops): %'15llu\n", global_counter);
		printf("Total blocks         : %'15llu\n", global_blocks);
		printf("Ops per second       : %'18.2lf\n", ((double)global_counter) / to_fseconds(end - start));
		printf("ns per ops           : %'18.2lf\n", ((double)(end - start)) / global_counter);
		printf("Ops per threads      : %'15llu\n", global_counter / nthreads);
		printf("Ops per procs        : %'15llu\n", global_counter / nprocs);
		printf("Ops/sec/procs        : %'18.2lf\n", (((double)global_counter) / nprocs) / to_fseconds(end - start));
		printf("ns per ops/procs     : %'18.2lf\n", ((double)(end - start)) / (global_counter / nprocs));
		fflush(stdout);
	}

	return 0;

}
