package main

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
	"golang.org/x/text/language"
	"golang.org/x/text/message"
)

func yielder(result chan uint64, start *sync.WaitGroup) {
	count := uint64(0)
	start.Wait()
	for true {
		runtime.Gosched()
		count += 1
		if  clock_mode && atomic.LoadInt32(&stop) == 1 { break }
		if !clock_mode && count >= stop_count { break }
	}

	atomic.AddInt64(&threads_left, -1);
	result <- count
}

func main() {
	bench_init()

	threads_left = int64(nthreads)

	result := make(chan uint64)
	var wg sync.WaitGroup
	wg.Add(1)

	for i := 0; i < nthreads; i++ {
		go yielder(result, &wg)
	}
	fmt.Printf("Starting\n");
	atomic.StoreInt32(&stop, 0)
	start := time.Now()
	wg.Done();
	wait(start, true);

	atomic.StoreInt32(&stop, 1)
	end := time.Now()
	duration := end.Sub(start)

	fmt.Printf("\nDone\n")

	global_counter := uint64(0)
	for i := 0; i < nthreads; i++ {
		global_counter += <- result
	}

	p := message.NewPrinter(language.English)
	p.Printf("Duration (ms)        : %d\n", duration.Milliseconds())
	p.Printf("Number of processors : %d\n", nprocs)
	p.Printf("Number of threads    : %d\n", nthreads)
	p.Printf("Total Operations(ops): %15d\n", global_counter)
	p.Printf("Ops per second       : %18.2f\n", float64(global_counter) / duration.Seconds())
	p.Printf("ns per ops           : %18.2f\n", float64(duration.Nanoseconds()) / float64(global_counter))
	p.Printf("Ops per threads      : %15d\n", global_counter / uint64(nthreads))
	p.Printf("Ops per procs        : %15d\n", global_counter / uint64(nprocs))
	p.Printf("Ops/sec/procs        : %18.2f\n", (float64(global_counter) / float64(nprocs)) / duration.Seconds())
	p.Printf("ns per ops/procs     : %18.2f\n", float64(duration.Nanoseconds()) / (float64(global_counter) / float64(nprocs)))
}