# AclXSched: Compute and Memory Scheduling for Ascend NPUs [WIP]

[![License](https://img.shields.io/badge/License-Apache_2.0-blue)](LICENSE)

## About

AclXSched brings flexible multitasking to Ascend NPUs.
Built on [XSched](docs/xsched-osdi25.pdf), it combines **preemptive compute scheduling** with **proactive memory scheduling**, so applications can share a NPU even when their combined memory demands exceed its capacity.
Existing Ascend applications run without source code changes, while users can choose scheduling policies to match their priorities and performance needs.

AclXSched proactively prepares the incoming task's working set at each context switch.
It predicts upcoming memory accesses using queued GPU commands and the execution schedule, then uses these predictions to guide eviction and load the working set before the task resumes.

### Features

- **Transparency**: Works with existing applications without code changes.
- **Preemptive compute scheduling**: Preempts running NPU tasks and switches between them to improve responsiveness and fairness.
Supports diverse scheduling policies.
- **Memory oversubscription:** Share NPU memory across applications whose combined memory footprint exceeds device capacity.
- **Predictive memory management:** Combine offline access templates to predict memory aceesses and coordinate page migration with task execution.

## Getting Started

### Build and Install

Requirements:

- Linux, Git, GNU Make, and CMake 3.14 or later
- A C++20 compiler (GCC recommended)
- An Ascend NPU with its driver and CANN toolkit/runtime installed

Run the following commands from the project root:

```bash
git submodule update --init --recursive

# Build the Ascend HAL, shim, and scheduling service.
# The default installation directory is output/ under the project root.
make ascend

# Alternatively, install to a custom path.
make ascend INSTALL_PATH=/path/to/install
```

### Use Cases

#### Transparently Schedule Ascend Applications

AclXSched is designed to be transparent to applications.
By loading the Ascend shim and setting a few [environment variables](protocol/README.md), you can schedule supported Ascend applications without modifying their source code.
For scheduling across processes, `xserver` coordinates the applications and `xcli` lets you monitor queues or adjust the policy through scheduling hints.
See the [Ascend usage guide](platforms/ascend/README.md#usage) and [service guide](service/README.md) for details.

#### Combined Compute and Memory Scheduling

The development goal is to use host memory for data that is not needed immediately and prepare each task's predicted working set before it resumes.
The Ascend adapter currently has no memory manager, access predictor, or execution-time profiler.
See the [Ascend support status](platforms/ascend/README.md#support) and the [development plan](#development-plan).

#### Customize Scheduling

Applications that need direct control can link with AclXSched to manage XQueues or provide hints to the scheduling policy.
See the [API guide](include/README.md) for the Ascend queue interface and shared scheduling APIs.

To implement a new scheduling policy, see the [policy guide](sched/README.md#how-to-implement-your-own-policy).
The [example list](examples/README.md) includes Ascend workloads and inherited XSched examples, with their target platforms identified.

## Architecture and Workflow

AclXSched maps each managed Ascend stream to an XQueue.
The Ascend shim, XPreempt, and Ascend HAL run in each application process, while `xserver` coordinates decisions across processes:

- **[Ascend shim](platforms/ascend):** intercepts supported AscendCL and ACLNN calls, creates XQueues for streams, and buffers asynchronous work for scheduling.
- **[XPreempt](preempt):** implements command submission, waiting, suspension, and resumption for XQueues.
- **[Ascend HAL](platforms/ascend):** launches buffered commands onto Ascend streams and synchronizes their completion using AscendCL events and stream APIs.
- **[Scheduler](sched):** decides which XQueues may submit work according to the selected policy, either locally or through `xserver`.

When an application submits a supported command, the shim enqueues it in the stream's XQueue.
The scheduler suspends or resumes command submission according to its policy.
Level-1 scheduling allows already launched commands to complete normally; it does not interrupt an executing NPU operator.

## Documentation

| Guide | Contents |
| ----- | -------- |
| [CUDA](platforms/cuda/README.md) | Running applications, profiling, and memory prediction |
| [XPreempt](preempt/README.md) | Command queues and memory contexts |
| [Scheduling](sched/README.md) | Scheduling policies and how to add a policy |
| [Configuration](protocol/README.md) | Environment variables and their defaults |
| [Public APIs](include/README.md) | Queue control, scheduling hints, and memory advice |
| [Service](service/README.md) | Using `xserver` and `xcli` |
| [Platform adapters](platforms/README.md) | Hardware abstraction and API interception |

The [XSched introduction](docs/xsched-intro-2025-en.md) and its [Chinese version](docs/xsched-intro-2025-zh.md) describe the underlying compute scheduling framework.

## Development Plan

We will continue to improve AclXSched's scheduling capabilities and support more Ascend NPUs and CANN versions.

- [ ] Implement memory oversubscription and host/device memory migration on Ascend NPUs.
- [ ] Support preemption beyond Level-1.
- [ ] Support spatial multiplexing to schedule NPU tasks running concurrently.
- [ ] Extend memory scheduling to more scheduling policies.

## Contributing

AclXSched is designed to be extensible and flexible.
We welcome contributions:

- Implement a new scheduling policy or extend an existing policy with memory scheduling. See the [guide](sched/README.md).
- Improve memory access prediction, NPU preemption, or CANN compatibility. See the [Ascend guide](platforms/ascend/README.md).
- Integrate AclXSched into Ascend applications and inference systems.
- Report or fix issues, and improve documentation and examples.

## Citation

If you use AclXSched for your research, please cite our papers:

```bibtex
@misc{Shen2026msched,
  title={Towards Fully-fledged GPU Multitasking via Proactive Memory Scheduling},
  author={Weihang Shen and Yinqiu Chen and Rong Chen and Haibo Chen},
  year={2026},
  eprint={2512.24637},
  archivePrefix={arXiv},
  primaryClass={cs.OS},
  url={https://arxiv.org/abs/2512.24637},
}

@inproceedings{Shen2025xsched,
  title = {{XSched}: Preemptive Scheduling for Diverse {XPU}s},
  author = {Weihang Shen and Mingcong Han and Jialong Liu and Rong Chen and Haibo Chen},
  booktitle = {19th USENIX Symposium on Operating Systems Design and Implementation (OSDI 25)},
  year = {2025},
  isbn = {978-1-939133-47-2},
  address = {Boston, MA},
  pages = {671--692},
  url = {https://www.usenix.org/conference/osdi25/presentation/shen-weihang},
  publisher = {USENIX Association},
  month = jul
}
```

## Contact Us

- For technical questions and feature requests, please submit via GitHub [Issues](https://github.com/XpuOS/aclxsched/issues).
- For collaborations and partnerships, please reach out at [rongchen@sjtu.edu.cn](mailto:rongchen@sjtu.edu.cn).
