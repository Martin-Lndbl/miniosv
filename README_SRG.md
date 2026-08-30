# Running the packaged apps

One application is linked into the kernel, so *which app* is a build-time choice and *which
program inside it* is a boot-time one:

```bash
make app=app/miniduckdb              # -> build/release.x64/loader.img
make app=app/llama.cpp arch=aarch64  # -> build/release.aarch64/loader.img
```

`scripts/run.py --args "<executable> [args...]"` writes those words into the boot image before
starting QEMU; the first word names the executable, the rest is its `argv`. Run with no `--args`
to print the list.

## Data disks

`--emulated-nvme FILE` attaches a file as NVMe controller 1, 2, … in the order given (0 is the
boot disk). The file is either an ext4 image, which the app mounts (`/db` for DuckDB, `/root` for
llama.cpp), or a plain file the app reads as a raw namespace, named `nvme:<id>`.

```bash
scripts/mkdata.sh build/data.img [staging-dir] [size]     # ext4 image, no root needed
```

After an unclean shutdown the image needs `fsck.ext4 -f -y build/data.img` — there is no journal.

## DuckDB: TPC-H

Executables: `duckdb` (upstream CLI), `benchmark` (upstream runner), `sql` (minimal `-d`/`-c`).

### Pre-generated parquet, any scale factor

`miniosv/mk-tpch.sh` builds a data image from directories of TPC-H parquet files — one
`tpch<SF>/` per scale factor, each with the eight tables as `<table>.parquet` — and generates a
benchmark group that queries them. Files are hardlinked into the staging tree when it lands on the
same filesystem, so nothing is copied twice.

```bash
make app=app/miniduckdb
app/miniduckdb/miniosv/mk-tpch.sh --src [tpch_data_folder] --img [data_img] 1 10

scripts/run.py --image-path build/duckdb.x64/loader.img -m 32G --emulated-nvme [data_img] \
    --args "benchmark --sf 10 benchmark/tpch-parquet/q06.benchmark"        # one query
scripts/run.py --image-path build/duckdb.x64/loader.img -m 32G --emulated-nvme [data_img] \
    --args "benchmark --sf 10 'benchmark/tpch-parquet/q[0-9]+.benchmark'"  # all 22
```

The scale factor is a run-time argument, so one image carrying several of them serves all. Each
query runs five times and prints a timing per run; `--threads=n` and `--memory_limit=n` bound the
engine, `--list` shows every benchmark found. A benchmark is named by its path, not by the `name`
line inside it, and the pattern is a full-match regex.

Answers ship for sf1 and sf100 only, so those runs verify their results and the rest are timed but
unchecked.

### Generating in the guest instead

```bash
scripts/mkdata.sh build/data.img
scripts/run.py -m 6G --emulated-nvme build/data.img \
    --args "duckdb /db/tpch.duckdb -c 'CALL dbgen(sf=0.1)' -c 'PRAGMA tpch(1)'"
```

`PRAGMA tpch(N)` runs query N (1–22), `FROM tpch_queries()` lists them. The database stays on the
image, so a later boot skips `dbgen`. Without `-c` the CLI gives an interactive prompt; `.quit`
exits.

### Limits worth knowing

- Boot arguments are capped at 447 bytes ([setargs.py:26](scripts/setargs.py#L26)).
- The CLI's `-f` and `-init` go through libc `fopen`, which always fails here. SQL reaches the CLI
  through `-c` only; anything longer belongs in a benchmark file, which is read through miniext.
- `glob()` is unsupported, so `read_parquet('/db/tpch1/*.parquet')` does not work — name each file.

## llama.cpp

Executables: `llama-cli`, `batched-bench`. Both `exit()` when done, which powers the VM off.

```bash
make app=app/llama.cpp
MODEL=$(app/llama.cpp/miniosv/get-model.sh llama-3.2-1b-q8)   # --list for the others

scripts/run.py -m 6G --emulated-nvme "$MODEL" \
    --args "llama-cli -m nvme:1 -no-cnv -p 'The capital of France is' -n 32 -t 4"

scripts/run.py -m 6G --emulated-nvme "$MODEL" \
    --args "batched-bench -m nvme:1 -c 2048 -npp 16,32 -ntg 128,256 -npl 1,4 -t 4"
```

The model can equally live on an ext4 image (`mkdata.sh` with a staging dir holding `models/`),
then `-m /root/models/<name>.gguf`.

### vAccel

Matmul offload to a host accelerator, built in by default (`conf_vaccel=0` for a CPU-only image):

```bash
make app=app/llama.cpp arch=aarch64 conf_vaccel=1
scripts/run.py --arch aarch64 --vaccel -m 6G \
    --emulated-nvme build/data.img --emulated-nvme "$MODEL" \
    --args "llama-cli -m nvme:2 -no-cnv -p 'What is a combustion engine' -n 32 -t 4"
```

Needs the QEMU from the `lros-qemu` flake (`nix develop` sets `$QEMU_VACCEL`) and
`config/mat_kernel_size.json` on the data disk — the backend reads `/root/config` to decide which
matrix shapes it may offload.
