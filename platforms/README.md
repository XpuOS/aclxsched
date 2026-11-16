# Platform Support for AclXSched

## Structure

Each platform adapter is split into two parts:

- `hal`: the XPU adapter layer (XAL). It implements XSched's `HwCommand` and `HwQueue` abstractions on top of the platform driver/runtime APIs.
- `shim`: the transparent interception layer (XShim). It intercepts application API calls and redirects asynchronous commands to XQueues when XSched is enabled.

AclXSched focuses on the Ascend adapter, which currently implements transparent Level-1 compute scheduling.
The shared memory scheduling infrastructure and CUDA adapter are inherited from NVXSched; Ascend memory scheduling is under development.
Some platforms also provide language bindings or platform-specific tests.

## XAL

The XAL library converts native XPU work, such as a kernel launch, memcpy, or operator execution, into `HwCommand` objects submitted to an `XQueue`.

Common `HwCommand` interfaces:

| Interface               | Description                                                                 |
| ----------------------- | --------------------------------------------------------------------------- |
| Synchronize()           | Wait for the launched command if hardware supports         |
| Synchronizable()        | Report whether synchronization is enabled for the command                   |
| EnableSynchronization() | Enable synchronization, for example by recording an event after the command |
| Predict()               | Predict execution time and memory accesses at XQueue submission             |

`HwQueue` is an abstraction of a real device queue, such as `aclrtStream` in AscendCL or `CUstream` in CUDA. Different functions support different preemption levels.

<table>
  <tr>
    <th align="center">Preemption Level</th>
    <th align="center">Interface</th>
    <th align="center">Description</th>
  </tr>
  <tr>
    <td align="center" rowspan="2">Level-1</td>
    <td align="center">Launch(HwCommand)</td>
    <td align="left">Launch a HwCommand to the HwQueue through the platform API</td>
  </tr>
  <tr>
    <td align="center">Synchronize()</td>
    <td align="left">Wait for all launched commands in the HwQueue to complete</td>
  </tr>
  <tr>
    <td align="center" rowspan="2">Level-2</td>
    <td align="center">Deactivate()</td>
    <td align="left">Deactivate the HwQueue to prevent all its commands from being selected for execution</td>
  </tr>
  <tr>
    <td align="center">Reactivate()</td>
    <td align="left">Reactivate the HwQueue to allow all its commands to be selected for execution</td>
  </tr>
  <tr>
    <td align="center" rowspan="2">Level-3</td>
    <td align="center">Interrupt()</td>
    <td align="left">Interrupt the running command of the HwQueue</td>
  </tr>
  <tr>
    <td align="center">Restore()</td>
    <td align="left">Restore the interrupted command of the HwQueue</td>
  </tr>
</table>

Only Level-1 is required for a usable platform adapter.

For adapters that flush or interrupt commands for Level-2 or 3 preemption, reaching a hardware synchronization point does not necessarily mean the original work has completed.
XPreempt keeps a command log so that the adapter can restore or relaunch unfinished work on resumption.
Sections 4 and 5 of the [XSched paper](../docs/xsched-osdi25.pdf) describe this hardware model and the preemption protocol.

### Memory Scheduling Interfaces

Memory scheduling extends the adapter with access prediction and memory management.
The current Ascend adapter does not implement these extensions; the interfaces below describe shared infrastructure retained for this work.
Commands provide their predicted accesses through `Predict()`, and an `HwMemoryManager` implements device allocation, release, memory advice, and page fault feedback.
XPreempt accesses this manager through the `XMemoryManager` associated with each HwQueue, which tracks allocations and applies advice to the predicted working set.

The inherited CUDA implementation uses managed allocations to move pages between GPU and host memory, UVM driver interfaces to control page placement, and CUPTI records to associate page faults with kernels.
See the [CUDA guide](cuda/README.md) for the implementation and driver requirements.

## XShim

The XShim library preserves application transparency.
It intercepts the platform API calls that create queues, submit asynchronous work, record or wait for events, synchronize, and release resources whose lifetime can affect queued work.

- `intercept.cpp` exports intercepted platform symbols and redirects them.
- `shim.cpp` implements XSched-specific handling for those symbols.

The inherited CUDA shim additionally redirects supported device allocations to managed memory, enabling memory oversubscription without application changes.
The current Ascend shim does not redirect allocations for oversubscription.

## Ascend Support

AscendCL is the primary adapter maintained in this repository.
See the [Ascend guide](ascend/README.md) for its support status, build instructions, and usage.

## Inherited Platform Adapters

The following adapters are retained from XSched / NVXSched as references.
Their configuration and examples are platform-specific; the inherited memory scheduling implementation targets CUDA on Linux.

| Name      | Usage                         |
| --------- | ----------------------------- |
| CUDA      | [README](cuda/README.md)      |
| CoreX     | [README](corex/README.md)     |
| HIP       | [README](hip/README.md)       |
| LevelZero | [README](levelzero/README.md) |
| OpenCL    | [README](opencl/README.md)    |
| cuDLA     | [README](cudla/README.md)     |
| VPI       | [README](vpi/README.md)       |

If you are interested in supporting XSched on a new platform, please refer to our [example and guide](example/README.md).
