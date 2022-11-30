package main

import (
	"flag"
	"fmt"
	"sync/atomic"
	"time"
	"golang.org/x/text/language"
	"golang.org/x/text/message"
)

func partner(result chan uint64, mine chan int, next chan int) {
	count := uint64(0)
	for true {
		<- mine
		select {
		case next <- 0:
		default:
		}
		count += 1
		if  clock_mode && atomic.LoadInt32(&stop) == 1 { break }
		if !clock_mode && count >= stop_count { break }
	}

	atomic.AddInt64(&threads_left, -1);
	result <- count
}

func main() {
	var ring_size int

	ring_sizeOpt := flag.Int("r", 2, "The number of threads per cycles")

	bench_init()

	ring_size = *ring_sizeOpt

	tthreads := nthreads * ring_size
	threads_left = int64(tthreads)

	result := make(chan uint64)
	channels := make([]chan int, tthreads)
	for i := range channels {
		channels[i] = make(chan int, 1)
	}

	for i := 0; i < tthreads; i++ {
		pi := (i + nthreads) % tthreads
		go partner(result, channels[i], channels[pi])
	}
	fmt.Printf("Starting\n");

	atomic.StoreInt32(&stop, 0)
	start := time.Now()
	for i := 0; i < nthreads; i++ {
		channels[i] <- 0
	}
	wait(start, true);

	atomic.StoreInt32(&stop, 1)
	end := time.Now()
	duration := end.Sub(start)

	fmt.Printf("\nDone\n")

	global_counter := uint64(0)
	for i := 0; i < tthreads; i++ {
		select {
		case channels[i] <- 0:
		default:
		}
		global_counter += <- result
	}

	p := message.NewPrinter(language.English)
	p.Printf("Duration (ms)        : %d\n", duration.Milliseconds())
	p.Printf("Number of processors : %d\n", nprocs);
	p.Printf("Number of threads    : %d\n", tthreads);
	p.Printf("Cycle size (# thrds) : %d\n", ring_size);
	p.Printf("Total Operations(ops): %15d\n", global_counter)
	p.Printf("Ops per second       : %18.2f\n", float64(global_counter) / duration.Seconds())
	p.Printf("ns per ops           : %18.2f\n", float64(duration.Nanoseconds()) / float64(global_counter))
	p.Printf("Ops per threads      : %15d\n", global_counter / uint64(tthreads))
	p.Printf("Ops per procs        : %15d\n", global_counter / uint64(nprocs))
	p.Printf("Ops/sec/procs        : %18.2f\n", (float64(global_counter) / float64(nprocs)) / duration.Seconds())
	p.Printf("ns per ops/procs     : %18.2f\n", float64(duration.Nanoseconds()) / (float64(global_counter) / float64(nprocs)))

}
