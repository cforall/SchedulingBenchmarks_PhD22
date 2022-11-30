package main

import (
	"flag"
	"fmt"
	"math/rand"
	"os"
	"regexp"
	"runtime"
	"sync/atomic"
	"time"
	"golang.org/x/text/language"
	"golang.org/x/text/message"
)

type LeaderInfo struct {
	id uint64
	idx uint64
	estop uint64
	seed uint64
}

func __xorshift64( state * uint64 ) (uint64) {
	x := *state
	x ^= x << 13
	x ^= x >> 7
	x ^= x << 17
	*state = x
	return x
}

func (this * LeaderInfo) next(len uint64) {
	n := __xorshift64( &this.seed )
	atomic.StoreUint64( &this.id, n % len )
}

func NewLeader(size uint64) (*LeaderInfo) {
	this := &LeaderInfo{0, 0, 0, uint64(os.Getpid())}

	r := rand.Intn(10)

	for i := 0; i < r; i++ {
		this.next( uint64(nthreads) )
	}

	return this
}

type MyThread struct {
	id uint64
	idx uint64
	sem chan struct{}
}

func waitgroup(leader * LeaderInfo, idx uint64, threads [] MyThread, main_sem chan struct {}) {
	start := time.Now()
	Outer:
	for i := 0; i < len(threads); i++ {
		// fmt.Fprintf(os.Stderr, "Waiting for :%d (%d)\n", threads[i].id, atomic.LoadUint64(&threads[i].idx) );
		for atomic.LoadUint64( &threads[i].idx ) != idx {
			// hint::spin_loop();
			end := time.Now()
			delta := end.Sub(start)
			if delta.Seconds() > 5 {
				fmt.Fprintf(os.Stderr, "Programs has been blocked for more than 5 secs")
				atomic.StoreUint64(&leader.estop, 1);
				main_sem <- (struct {}{})
				break Outer
			}
		}
	}
	// debug!( "Waiting done" );
}

func wakegroup(exhaust bool, me uint64, threads [] MyThread) {
	if !exhaust { return; }

	for i := uint64(0); i < uint64(len(threads)); i++ {
		if i != me {
			// debug!( "Leader waking {}", i);
			defer func() {
				if err := recover(); err != nil {
					fmt.Fprintf(os.Stderr, "Panic occurred: %s\n", err)
				}
			}()
			threads[i].sem <- (struct {}{})
		}
	}
}

func lead(exhaust bool, leader * LeaderInfo, this * MyThread, threads [] MyThread, main_sem chan struct {}) {
	nidx := atomic.LoadUint64(&leader.idx) + 1;
	atomic.StoreUint64(&this.idx, nidx);
	atomic.StoreUint64(&leader.idx, nidx);

	if nidx > stop_count || atomic.LoadUint64(&leader.estop) != 0 {
		// debug!( "Leader {} done", this.id);
		main_sem <- (struct {}{})
		return
	}

	// debug!( "====================\nLeader no {} : {}", nidx, this.id);

	waitgroup(leader, nidx, threads, main_sem);

	leader.next( uint64(len(threads)) );

	wakegroup(exhaust, this.id, threads);

	// debug!( "Leader no {} : {} done\n====================", nidx, this.id);
}

func waitleader(exhaust bool, leader * LeaderInfo, this * MyThread, rechecks * uint64) {
	runtime.Gosched()

	if atomic.LoadUint64(&leader.idx) == atomic.LoadUint64(&this.idx) {
		// debug!("Waiting {} recheck", this.id);
		*rechecks += uint64(1)
		return
	}

	// debug!("Waiting {}", this.id);

	// debug_assert!( (leader.idx.load(Ordering::Relaxed) - 1) == this.idx.load(Ordering::Relaxed) );
	atomic.AddUint64(&this.idx, 1)
	if exhaust {
		// debug!("Waiting {} sem", this.id);
		<- this.sem
	} else {
		// debug!("Waiting {} yield", this.id);
		runtime.Gosched()
	}

	// debug!("Waiting {} done", this.id);
}

func transfer_main( result chan uint64, me uint64, leader * LeaderInfo, threads [] MyThread, exhaust bool, start chan struct{}, main_sem chan struct{}) {
	// assert!( me == threads[me].id );

	// debug!("Ready {}: {:p}", me, &threads[me].sem as *const sync::Semaphore);

	// Wait for start
	<- start

	// debug!( "Start {}", me );

	// Prepare results
	r := uint64(0)

	// Main loop
	for true {
		if atomic.LoadUint64(&leader.id) == me {
			lead( exhaust, leader, &threads[me], threads, main_sem )
			runtime.Gosched()
		} else {
			waitleader( exhaust, leader, &threads[me], &r )
		}
		if atomic.LoadUint64(&leader.idx) > stop_count || atomic.LoadUint64(&leader.estop) != 0 { break; }
	}

	// return result
	result <- r
}

func main() {
	// Benchmark specific command line arguments
	exhaustOpt := flag.String("e", "no", "Whether or not threads that have seen the new epoch should park instead of yielding.")

	// General benchmark initialization and deinitialization
	bench_init()

	exhaustVal := *exhaustOpt;

	var exhaust bool
	re_yes := regexp.MustCompile("[Yy]|[Yy][Ee][Ss]")
	re_no  := regexp.MustCompile("[Nn]|[Nn][Oo]")
	if re_yes.Match([]byte(exhaustVal)) {
		exhaust = true
	} else if re_no.Match([]byte(exhaustVal)) {
		exhaust = false
	} else {
		fmt.Fprintf(os.Stderr, "Unrecognized exhaust(-e) option '%s'\n", exhaustVal)
		os.Exit(1)
	}

	if clock_mode {
		fmt.Fprintf(os.Stderr, "Programs does not support fixed duration mode\n")
		os.Exit(1)
	}

	var es string
	if exhaust {
		es = "with"
	} else {
		es = "without"
	}
	fmt.Printf("Running %d threads on %d processors, doing %d iterations, %s exhaustion\n", nthreads, nprocs, stop_count, es );

	main_sem := make(chan struct{})
	leader := NewLeader(uint64(nthreads))
	barr := make(chan struct{})
	result := make(chan uint64)

	thddata := make([]MyThread, nthreads)
	for i := range thddata {
		thddata[i] = MyThread{ uint64(i), 0, make(chan struct {}) }
	}

	rechecks := uint64(0)
	for i := range thddata {
		go transfer_main(result, uint64(i), leader, thddata, exhaust, barr, main_sem)
	}
	fmt.Printf("Starting\n");

	start := time.Now()
	close(barr) // release barrier

	<- main_sem

	end := time.Now()
	delta := end.Sub(start)

	fmt.Printf("\nDone\n")

	// release all the blocked threads
	for i := range thddata {
		close(thddata[i].sem)
	}
	for range thddata {
		rechecks += <- result
	}

	p := message.NewPrinter(language.English)
	var ws string
	if exhaust {
		ws = "yes"
	} else {
		ws = "no"
	}
	p.Printf("Duration (ms)           : %d\n", delta.Milliseconds() )
	p.Printf("Number of processors    : %d\n", nprocs )
	p.Printf("Number of threads       : %d\n", nthreads )
	p.Printf("Total Operations(ops)   : %15d\n", (leader.idx - 1) )
	p.Printf("Threads parking on wait : %s\n", ws)
	p.Printf("Rechecking              : %d\n", rechecks )
	p.Printf("ms per transfer         : %f\n", float64(delta.Milliseconds()) / float64(leader.idx) )
}
