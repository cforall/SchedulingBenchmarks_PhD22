use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Instant;

use tokio::runtime::Builder;
use tokio::sync;
use tokio::time::{sleep,Duration};

use rand::Rng;

use clap::{Arg, App};
use num_format::{Locale, ToFormattedString};

#[path = "../bench.rs"]
mod bench;

// ==================================================
struct Churner {
	start: sync::Notify,
}

async fn churn_main( this: Arc<Churner>, spots: Arc<Vec<sync::Semaphore>>, exp: Arc<bench::BenchData>, skip_in: bool ) -> u64 {
	let mut skip = skip_in;
	this.start.notified().await;

	let mut count:u64 = 0;
	loop {
		let r : usize = rand::thread_rng().gen();

		let spot : &sync::Semaphore = &spots[r % spots.len()];
		if !skip { spot.add_permits(1); }
		spot.acquire().await.forget();
		skip = false;

		count += 1;
		if  exp.clock_mode && exp.stop.load(Ordering::Relaxed) { break; }
		if !exp.clock_mode && count >= exp.stop_count { break; }
	}

	exp.threads_left.fetch_sub(1, Ordering::SeqCst);
	count
}

// ==================================================
fn main() {
	let options = App::new("Churn Tokio")
		.args(&bench::args())
		.arg(Arg::with_name("nspots")  .short("s").long("spots")  .takes_value(true).default_value("1").help("Number of spots in the system"))
		.get_matches();

	let nthreads  = options.value_of("nthreads").unwrap().parse::<usize>().unwrap();
	let nprocs    = options.value_of("nprocs").unwrap().parse::<usize>().unwrap();
	let nspots    = options.value_of("nspots").unwrap().parse::<usize>().unwrap();

	let exp = Arc::new(bench::BenchData::new(options, nthreads, None));

	let s = (1000000 as u64).to_formatted_string(&Locale::en);
	assert_eq!(&s, "1,000,000");

	let spots : Arc<Vec<sync::Semaphore>> = Arc::new((0..nspots).map(|_| {
		sync::Semaphore::new(0)
	}).collect());

	let thddata : Vec<Arc<Churner>> = (0..nthreads).map(|_| {
		Arc::new(Churner{
			start: sync::Notify::new(),
		})
	}).collect();

	let mut global_counter :u64 = 0;
	let mut duration : std::time::Duration = std::time::Duration::from_secs(0);
	let runtime = Builder::new_multi_thread()
		.worker_threads(nprocs)
		.enable_all()
		.build()
		.unwrap();

	runtime.block_on(async {
		let threads: Vec<_> = (0..nthreads).map(|i| {
			tokio::spawn(churn_main(thddata[i].clone(), spots.clone(), exp.clone(), i < nspots))
		}).collect();
		println!("Starting");

		sleep(Duration::from_millis(100)).await;

		let start = Instant::now();

		for i in 0..nthreads {
			thddata[i].start.notify_one();
		}

		duration = exp.wait(&start).await;

		println!("\nDone");

		for i in 0..nspots {
			spots[i].add_permits(10000);
		}

		for t in threads {
			let c = t.await.unwrap();
			global_counter += c;
		}
	});

	println!("Duration (ms)        : {}", (duration.as_millis()).to_formatted_string(&Locale::en));
	println!("Number of processors : {}", (nprocs).to_formatted_string(&Locale::en));
	println!("Number of threads    : {}", (nthreads).to_formatted_string(&Locale::en));
	println!("Number of spots      : {}", "6");
	println!("Total Operations(ops): {:>15}", (global_counter).to_formatted_string(&Locale::en));
	println!("Ops per second       : {:>15}", (((global_counter as f64) / duration.as_secs() as f64) as u64).to_formatted_string(&Locale::en));
	println!("ns per ops           : {:>15}", ((duration.as_nanos() as f64 / global_counter as f64) as u64).to_formatted_string(&Locale::en));
	println!("Ops per threads      : {:>15}", (global_counter / nthreads as u64).to_formatted_string(&Locale::en));
	println!("Ops per procs        : {:>15}", (global_counter / nprocs as u64).to_formatted_string(&Locale::en));
	println!("Ops/sec/procs        : {:>15}", ((((global_counter as f64) / nprocs as f64) / duration.as_secs() as f64) as u64).to_formatted_string(&Locale::en));
	println!("ns per ops/procs     : {:>15}", ((duration.as_nanos() as f64 / (global_counter as f64 / nprocs as f64)) as u64).to_formatted_string(&Locale::en));
}
