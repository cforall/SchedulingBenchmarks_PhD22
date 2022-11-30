use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Instant;

use tokio::runtime::Builder;
use tokio::sync;
use tokio::task;

use clap::App;
use num_format::{Locale, ToFormattedString};

#[path = "../bench.rs"]
mod bench;

// ==================================================
struct Yielder {
	sem: sync::Semaphore,
}

async fn yield_main(idx: usize, others: Arc<Vec<Arc<Yielder>>>, exp: Arc<bench::BenchData> ) -> u64 {
	let this = &others[idx];
	this.sem.acquire().await.forget();

	let mut count:u64 = 0;
	loop {
		task::yield_now().await;
		count += 1;

		if  exp.clock_mode && exp.stop.load(Ordering::Relaxed) { break; }
		if !exp.clock_mode && count >= exp.stop_count { break; }
	}

	exp.threads_left.fetch_sub(1, Ordering::SeqCst);
	count
}

// ==================================================
fn main() {
	let options = App::new("Yield Tokio")
		.args(&bench::args())
		.get_matches();

	let nthreads  = options.value_of("nthreads").unwrap().parse::<usize>().unwrap();
	let nprocs    = options.value_of("nprocs").unwrap().parse::<usize>().unwrap();

	let exp = Arc::new(bench::BenchData::new(options, nthreads, None));

	let s = (1000000 as u64).to_formatted_string(&Locale::en);
	assert_eq!(&s, "1,000,000");

	let thddata : Arc<Vec<Arc<Yielder>>> = Arc::new(
		(0..nthreads).map(|_i| {
			Arc::new(Yielder{
				sem: sync::Semaphore::new(0),
			})
		}).collect()
	);

	let mut global_counter :u64 = 0;
	let mut duration : std::time::Duration = std::time::Duration::from_secs(0);
	let runtime = Builder::new_multi_thread()
		.worker_threads(nprocs)
		.enable_all()
		.build()
		.unwrap();

	runtime.block_on(async {
		let threads: Vec<_> = (0..nthreads).map(|i| {
			tokio::spawn(yield_main(i, thddata.clone(), exp.clone()))
		}).collect();
		println!("Starting");

		let start = Instant::now();

		for i in 0..nthreads {
			thddata[i].sem.add_permits(1);
		}

		duration = exp.wait(&start).await;

		println!("\nDone");

		for i in 0..nthreads {
			thddata[i].sem.add_permits(1);
		}

		for t in threads {
			global_counter += t.await.unwrap();
		}
	});

	println!("Duration (ms)        : {}", (duration.as_millis()).to_formatted_string(&Locale::en));
	println!("Number of processors : {}", (nprocs).to_formatted_string(&Locale::en));
	println!("Number of threads    : {}", (nthreads).to_formatted_string(&Locale::en));
	println!("Total Operations(ops): {:>15}", (global_counter).to_formatted_string(&Locale::en));
	println!("Ops per second       : {:>15}", (((global_counter as f64) / duration.as_secs() as f64) as u64).to_formatted_string(&Locale::en));
	println!("ns per ops           : {:>15}", ((duration.as_nanos() as f64 / global_counter as f64) as u64).to_formatted_string(&Locale::en));
	println!("Ops per threads      : {:>15}", (global_counter / nthreads as u64).to_formatted_string(&Locale::en));
	println!("Ops per procs        : {:>15}", (global_counter / nprocs as u64).to_formatted_string(&Locale::en));
	println!("Ops/sec/procs        : {:>15}", ((((global_counter as f64) / nprocs as f64) / duration.as_secs() as f64) as u64).to_formatted_string(&Locale::en));
	println!("ns per ops/procs     : {:>15}", ((duration.as_nanos() as f64 / (global_counter as f64 / nprocs as f64)) as u64).to_formatted_string(&Locale::en));
}
