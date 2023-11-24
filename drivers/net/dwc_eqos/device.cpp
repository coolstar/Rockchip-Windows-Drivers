#include "precomp.h"
#include "device.h"
#include "rxqueue.h"
#include "txqueue.h"
#include "queue_common.h"
#include "registers.h"
#include "trace.h"

#include <bcrypt.h>

/*
Lifecycle:

PrepareHardware     ReleaseHardware
D0Entry             D0Exit
(InterruptEnable)   (InterruptDisable)
CreateRxQueue       DestroyCallback
CreateTxQueue       DestroyCallback
(PacketQueueStart)  (PacketQueueCancel, PacketQueueStop)
(DisarmWake)        (ArmWake)
*/

static auto constexpr DefaultAxiMaxWriteOutstanding = 4u;
static auto constexpr DefaultAxiMaxReadOutstanding = 8u;
static auto constexpr DefaultCsrRate = 125'000'000u;
static auto constexpr BusBytes = 8u;
static auto constexpr LinkStatusBit = 0x80000000u;

enum InterruptsWanted : char
{
    InterruptsNone  = 0,
    InterruptsState = 1 << 0, // mac.LinkStatus, ch0.AbnormalInterruptSummary, ch0.FatalBusError
    InterruptsRx    = 1 << 1, // ch0.Rx
    InterruptsTx    = 1 << 2, // ch0.Tx
    InterruptsAll   = -1,
};

struct DeviceContext
{
    // Const after initialization.

    MacRegisters* regs;
    NETADAPTER adapter;
    WDFSPINLOCK lock;
    WDFINTERRUPT interrupt;
    WDFDMAENABLER dma;
    MacHwFeature0_t feature0;
    MacHwFeature1_t feature1;
    MacHwFeature2_t feature2;
    MacHwFeature3_t feature3;
    UINT8 permanentMacAddress[ETHERNET_LENGTH_OF_ADDRESS];
    UINT8 currentMacAddress[ETHERNET_LENGTH_OF_ADDRESS];

    // Mutable.

    LONG interruptStatus; // = ChannelStatus_t, plus top bit is LinkStatus. Interlocked update.

    InterruptsWanted interruptsWanted;  // Guarded by lock.
    NETPACKETQUEUE rxQueue;             // Guarded by lock.
    NETPACKETQUEUE txQueue;             // Guarded by lock.

    // Diagnostics/statistics.

    UINT32 isrHandled; // Updated only in ISR.
    UINT32 isrIgnored; // Updated only in ISR.
    UINT32 dpcLinkState; // Updated only in DPC.
    UINT32 dpcRx; // Updated only in DPC.
    UINT32 dpcTx; // Updated only in DPC.
    UINT32 dpcAbnormalStatus; // Updated only in DPC.
    UINT32 dpcFatalBusError; // Updated only in DPC.
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, DeviceGetContext)

struct AdapterContext
{
    WDFDEVICE device;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, AdapterGetContext)

static void
SetOneMacAddress(_Inout_ MacRegisters* regs, unsigned index, _In_reads_(6) UINT8 const* addr, bool enable)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    MacAddressLow_t regLo = {};
    regLo.Addr0 = addr[0];
    regLo.Addr1 = addr[1];
    regLo.Addr2 = addr[2];
    regLo.Addr3 = addr[3];

    MacAddressHigh_t regHi = {};
    regHi.Addr4 = addr[4];
    regHi.Addr5 = addr[5];
    regHi.AddressEnable = enable;

    Write32(&regs->MacAddress[index].High, regHi);
    Write32(&regs->MacAddress[index].Low, regLo);

    TraceEntryExit(SetOneMacAddress, LEVEL_VERBOSE,
        TraceLoggingUInt32(index),
        TraceLoggingHexInt32(regHi.Value32, "MacHi"),
        TraceLoggingHexInt32(regLo.Value32, "MacLo"));
}

// Perform a software reset, then set mac address 0 to the specified value.
// Returns either STATUS_SUCCESS or STATUS_TIMEOUT.
_IRQL_requires_(PASSIVE_LEVEL)
__declspec(code_seg("PAGE"))
static NTSTATUS
DeviceReset(_Inout_ MacRegisters* regs, _In_reads_(6) UINT8 const* mac0)
{
    // PASSIVE_LEVEL
    PAGED_CODE();

    Write32(&regs->DmaMode, 1); // Software reset.

    unsigned retry;
    for (retry = 1000u; retry != 0; retry -= 1)
    {
        KeStallExecutionProcessor(20);
        auto const dmaMode = Read32(&regs->DmaMode);
        if (0 == (dmaMode & 1))
        {
            SetOneMacAddress(regs, 0, mac0, true);
            TraceEntryExit(DeviceReset, LEVEL_INFO,
                TraceLoggingUInt32(retry));
            return STATUS_SUCCESS;
        }
    }

    TraceWrite("DeviceReset-timeout", LEVEL_ERROR);
    return STATUS_TIMEOUT;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static void
UpdateLinkState(_In_ DeviceContext const* context)
{
    // DISPATCH_LEVEL
    auto const controlStatus = Read32(&context->regs->MacPhyIfControlStatus); // Clears LinkStatus interrupt.
    auto const oldConfig = Read32(&context->regs->MacConfiguration);
    auto newConfig = oldConfig;
    newConfig.FullDuplex = controlStatus.FullDuplex;

    UINT32 speed;
    switch (controlStatus.Speed)
    {
    case PhyIfSpeed_2_5M:
        speed = 10'000'000u;
        newConfig.PortSelectSpeed = PortSelectSpeed_10M;
        break;
    case PhyIfSpeed_25M:
        speed = 100'000'000u;
        newConfig.PortSelectSpeed = PortSelectSpeed_100M;
        break;
    case PhyIfSpeed_125M:
        speed = 1'000'000'000u;
        newConfig.PortSelectSpeed = PortSelectSpeed_1000M;
        break;
    default:
        speed = 0;
        break;
    }

    // TODO: I think this is where we want to call ACPI to change phy clock speed.

    if (oldConfig.Value32 != newConfig.Value32)
    {
        Write32(&context->regs->MacConfiguration, newConfig);
    }

    NET_ADAPTER_LINK_STATE linkState;
    NET_ADAPTER_LINK_STATE_INIT(
        &linkState,
        speed,
        controlStatus.LinkUp ? MediaConnectStateConnected : MediaConnectStateDisconnected,
        controlStatus.FullDuplex ? MediaDuplexStateFull : MediaDuplexStateHalf,
        NetAdapterPauseFunctionTypeUnsupported, // TODO: Pause functions?
        NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated |
        NetAdapterAutoNegotiationFlagRcvLinkSpeedautoNegotiated |
        NetAdapterAutoNegotiationFlagDuplexAutoNegotiated);
    NetAdapterSetLinkState(context->adapter, &linkState);

    TraceEntryExit(UpdateLinkState, LEVEL_INFO,
        TraceLoggingHexInt32(controlStatus.Value32, "PhyIfControlStatus"),
        TraceLoggingHexInt32(oldConfig.Value32, "OldMacConfig"),
        TraceLoggingHexInt32(newConfig.Value32, "NewMacConfig"));
}

// Cleared by reading MacPhyIfControlStatus.
_IRQL_requires_max_(HIGH_LEVEL)
static MacInterruptEnable_t
MakeMacInterruptEnable(char interruptsWanted)
{
    // HIGH_LEVEL
    MacInterruptEnable_t interruptEnable = {};
    interruptEnable.LinkStatus = 0 != (interruptsWanted & InterruptsState);
    return interruptEnable;
}

// Cleared by writing Channel.Status.
_IRQL_requires_max_(HIGH_LEVEL)
static ChannelInterruptEnable_t
MakeChannelInterruptEnable(char interruptsWanted)
{
    // HIGH_LEVEL
    ChannelInterruptEnable_t interruptEnable = {};
    interruptEnable.Rx = 0 != (interruptsWanted & InterruptsRx);
    interruptEnable.Tx = 0 != (interruptsWanted & InterruptsTx);
    interruptEnable.NormalInterruptSummary = 1;
    interruptEnable.FatalBusError = 0 != (interruptsWanted & InterruptsState);
    interruptEnable.AbnormalInterruptSummary = 0 != (interruptsWanted & InterruptsState);
    return interruptEnable;
}

_IRQL_requires_(DISPATCH_LEVEL)
static void
DeviceInterruptEnable_Locked(_Inout_ DeviceContext* context, InterruptsWanted bitsToEnable)
{
    // DISPATCH_LEVEL
    auto const oldWanted = context->interruptsWanted;
    auto const newWanted = static_cast<InterruptsWanted>(oldWanted | bitsToEnable);
    if (oldWanted != newWanted)
    {
        context->interruptsWanted = newWanted;
        Write32(&context->regs->MacInterruptEnable, MakeMacInterruptEnable(newWanted));
        Write32(&context->regs->DmaCh[0].InterruptEnable, MakeChannelInterruptEnable(newWanted));
        TraceEntryExit(DeviceInterruptEnable, LEVEL_VERBOSE,
            TraceLoggingHexInt32(oldWanted, "old"),
            TraceLoggingHexInt32(newWanted, "new"));
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static void
DeviceInterruptEnable(_Inout_ DeviceContext* context, InterruptsWanted bitsToEnable)
{
    // DISPATCH_LEVEL
    WdfSpinLockAcquire(context->lock);
    DeviceInterruptEnable_Locked(context, bitsToEnable);
    WdfSpinLockRelease(context->lock);
}

_IRQL_requires_(DISPATCH_LEVEL)
static void
DeviceInterruptDisable_Locked(_Inout_ DeviceContext* context, InterruptsWanted bitsToDisable)
{
    // DISPATCH_LEVEL
    auto const oldWanted = context->interruptsWanted;
    auto const newWanted = static_cast<InterruptsWanted>(oldWanted & ~bitsToDisable);
    if (oldWanted != newWanted)
    {
        context->interruptsWanted = newWanted;
        Write32(&context->regs->MacInterruptEnable, MakeMacInterruptEnable(newWanted));
        Write32(&context->regs->DmaCh[0].InterruptEnable, MakeChannelInterruptEnable(newWanted));
        TraceWrite("DeviceInterruptDisable", LEVEL_VERBOSE,
            TraceLoggingHexInt32(oldWanted, "old"),
            TraceLoggingHexInt32(newWanted, "new"));
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static void
DeviceInterruptDisable(_Inout_ DeviceContext* context, InterruptsWanted bitsToEnable)
{
    // DISPATCH_LEVEL
    WdfSpinLockAcquire(context->lock);
    DeviceInterruptDisable_Locked(context, bitsToEnable);
    WdfSpinLockRelease(context->lock);
}

static EVT_WDF_INTERRUPT_ISR DeviceInterruptIsr;
static BOOLEAN
DeviceInterruptIsr(
    _In_ WDFINTERRUPT interrupt,
    _In_ ULONG messageId)
{
    // HIGH_LEVEL
    UNREFERENCED_PARAMETER(messageId);
    auto const context = DeviceGetContext(WdfInterruptGetDevice(interrupt));

    auto const mac = Read32(&context->regs->MacInterruptStatus);
    auto const channel0 = Read32(&context->regs->DmaCh[0].Status);
    if (mac.LinkStatus || channel0.Value32 != 0)
    {
        if (mac.LinkStatus)
        {
            Read32(&context->regs->MacPhyIfControlStatus); // Clears LinkStatus.
        }

        if (channel0.Value32 != 0)
        {
            Write32(&context->regs->DmaCh[0].Status, channel0); // Clears DmaCh0.Status.
        }

        InterlockedOrNoFence(
            &context->interruptStatus,
            channel0.Value32 | (mac.LinkStatus ? LinkStatusBit : 0u));
        WdfInterruptQueueDpcForIsr(interrupt);

        context->isrHandled += 1;
        return true;
    }

    context->isrIgnored += 1;
    return false;
}

static EVT_WDF_INTERRUPT_DPC DeviceInterruptDpc;
static void
DeviceInterruptDpc(
    _In_ WDFINTERRUPT interrupt,
    _In_ WDFOBJECT associatedObject)
{
    // DISPATCH_LEVEL
    UNREFERENCED_PARAMETER(interrupt);
    auto const context = DeviceGetContext(associatedObject);
    NT_ASSERT(context->interrupt == interrupt);

    for (;;)
    {
        ChannelStatus_t const status{ (ULONG)InterlockedExchangeNoFence(&context->interruptStatus, 0) };
        if (status.Value32 == 0)
        {
            break;
        }

        if (status.AbnormalInterruptSummary || status.FatalBusError)
        {
            // TODO - error recovery?
            context->dpcAbnormalStatus += status.AbnormalInterruptSummary;
            context->dpcFatalBusError += status.FatalBusError;
            TraceWrite("DeviceInterruptDpc-ERROR", LEVEL_ERROR,
                TraceLoggingHexInt32(status.Value32, "status"));
        }
        else
        {
            TraceWrite("DeviceInterruptDpc", LEVEL_VERBOSE,
                TraceLoggingHexInt32(status.Value32, "status"));
        }

        if (status.Value32 & LinkStatusBit)
        {
            context->dpcLinkState += 1;
            UpdateLinkState(context);
        }

        auto const interruptsRxTx = static_cast<InterruptsWanted>(
            (status.Rx ? InterruptsRx : InterruptsNone) |
            (status.Tx ? InterruptsTx : InterruptsNone));
        if (interruptsRxTx != 0)
        {
            WdfSpinLockAcquire(context->lock);

            DeviceInterruptDisable_Locked(context, interruptsRxTx);

            if (status.Rx && context->rxQueue)
            {
                context->dpcRx += 1;
                NetRxQueueNotifyMoreReceivedPacketsAvailable(context->rxQueue);
                context->rxQueue = nullptr;
            }

            if (status.Tx && context->txQueue)
            {
                context->dpcTx += 1;
                NetTxQueueNotifyMoreCompletedPacketsAvailable(context->txQueue);
                context->txQueue = nullptr;
            }

            WdfSpinLockRelease(context->lock);
        }
    }
}

static EVT_NET_ADAPTER_CREATE_TXQUEUE AdapterCreateTxQueue;
static NTSTATUS
AdapterCreateTxQueue(
    _In_ NETADAPTER adapter,
    _Inout_ NETTXQUEUE_INIT* queueInit)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    auto const context = DeviceGetContext(AdapterGetContext(adapter)->device);
    NT_ASSERT(context->txQueue == nullptr);
    return TxQueueCreate(
        adapter,
        queueInit,
        context->dma,
        &context->regs->DmaCh[0],
        &context->regs->MtlQ[0]);
}

static EVT_NET_ADAPTER_CREATE_RXQUEUE AdapterCreateRxQueue;
static NTSTATUS
AdapterCreateRxQueue(
    _In_ NETADAPTER adapter,
    _Inout_ NETRXQUEUE_INIT* queueInit)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    auto const context = DeviceGetContext(AdapterGetContext(adapter)->device);
    NT_ASSERT(context->rxQueue == nullptr);
    return RxQueueCreate(
        adapter,
        queueInit,
        context->dma,
        &context->regs->DmaCh[0]);
}

static EVT_NET_ADAPTER_SET_RECEIVE_FILTER AdapterSetReceiveFilter;
static void
AdapterSetReceiveFilter(
    _In_ NETADAPTER adapter,
    _In_ NETRECEIVEFILTER receiveFilter)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    TraceEntry(AdapterSetReceiveFilter, LEVEL_INFO);
    auto const context = DeviceGetContext(AdapterGetContext(adapter)->device);

    auto const flags = NetReceiveFilterGetPacketFilter(receiveFilter);
    auto const mcastCount = (flags & NetPacketFilterFlagMulticast)
        ? NetReceiveFilterGetMulticastAddressCount(receiveFilter)
        : 0;
    auto const mcast = mcastCount > 0
        ? NetReceiveFilterGetMulticastAddressList(receiveFilter)
        : nullptr;

    MacPacketFilter_t filter = {};
    if (flags & NetPacketFilterFlagPromiscuous)
    {
        filter.PromiscuousMode = true;
    }
    else
    {
        filter.PassAllMulticast = 0 != (flags & NetPacketFilterFlagAllMulticast);
        filter.DisableBroadcast = 0 == (flags & NetPacketFilterFlagBroadcast);

        SetOneMacAddress(context->regs, 0, context->currentMacAddress,
            0 != (flags & NetPacketFilterFlagDirected)); // Address[0] can't really be disabled...

        // Could also use hash-based filtering for additional mcast support, but this seems okay.
        auto const macAddrCount = context->feature0.MacAddrCount;
        for (unsigned i = 1; i < macAddrCount; i += 1)
        {
            static constexpr UINT8 zero[ETHERNET_LENGTH_OF_ADDRESS] = {};
            bool const enable = mcastCount > i - 1 && mcast[i - 1].Length >= ETHERNET_LENGTH_OF_ADDRESS;
            auto const addr = enable ? mcast[i - 1].Address : zero;
            SetOneMacAddress(context->regs, i, addr, enable);
        }
    }

    Write32(&context->regs->MacPacketFilter, filter);

    TraceExit(AdapterSetReceiveFilter, LEVEL_INFO,
        TraceLoggingHexInt32(flags),
        TraceLoggingUIntPtr(mcastCount));
}

static EVT_WDF_DEVICE_D0_ENTRY DeviceD0Entry;
static NTSTATUS
DeviceD0Entry(
    _In_ WDFDEVICE device,
    _In_ WDF_POWER_DEVICE_STATE previousState)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    NTSTATUS status = STATUS_SUCCESS;
    auto const context = DeviceGetContext(device);

    // TX configuration.

    MacTxFlowCtrl_t txFlowCtrl = {};
    txFlowCtrl.TransmitFlowControlEnable = true;
    txFlowCtrl.PauseTime = 0xFFFF;
    Write32(&context->regs->MacTxFlowCtrl, txFlowCtrl); // TxFlow control, pause time.

    MtlTxOperationMode_t txOperationMode = {};
    txOperationMode.StoreAndForward = true;
    txOperationMode.QueueEnable = MtlTxQueueEnable_Enabled;
    txOperationMode.QueueSize = (128u << context->feature1.TxFifoSize) / 256u - 1; // Use 100% of FIFO. (TODO: Not sure about the -1.)
    Write32(&context->regs->MtlQ[0].TxOperationMode, txOperationMode);

    // RX configuration.

    Write32(&context->regs->MacRxFlowCtrl, 0x3); // Rx flow control, pause packet detect.
    Write32(&context->regs->MacRxCtrl0, 0x2); // RxQ0 enabled for DCB/generic.

    MtlRxOperationMode_t rxOperationMode = {};
    rxOperationMode.StoreAndForward = true;
    rxOperationMode.ForwardErrorPackets = true;
    rxOperationMode.ForwardUndersizedGoodPackets = true;
    rxOperationMode.QueueSize = (128u << context->feature1.RxFifoSize) / 256u - 1; // Use 100% of FIFO. (TODO: Not sure about the -1.)
    rxOperationMode.HardwareFlowControl = true;
    rxOperationMode.FlowControlActivate = 2; // Full - 2KB
    rxOperationMode.FlowControlDeactivate = 10; // Full - 6KB
    Write32(&context->regs->MtlQ[0].RxOperationMode, rxOperationMode);

    // MAC configuration.

    MacConfiguration_t macConfig = {};
    macConfig.DisableCarrierSenseDuringTransmit = true;
    macConfig.PacketBurstEnable = true;
    macConfig.ReceiverEnable = true;
    macConfig.TransmitterEnable = true;
    Write32(&context->regs->MacConfiguration, macConfig);

    // Clear and then enable interrupts.
    UpdateLinkState(context);
    (void)Read32(&context->regs->DmaCh[0].Status);
    DeviceInterruptEnable(context, InterruptsState);

    TraceEntryExitWithStatus(DeviceD0Entry, LEVEL_INFO, status,
        TraceLoggingUInt32(previousState));
    return status;
}

static EVT_WDF_DEVICE_D0_EXIT DeviceD0Exit;
__declspec(code_seg("PAGE"))
static NTSTATUS
DeviceD0Exit(
    _In_ WDFDEVICE device,
    _In_ WDF_POWER_DEVICE_STATE targetState)
{
    // PASSIVE_LEVEL
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;
    auto const context = DeviceGetContext(device);

    DeviceInterruptDisable(context, InterruptsAll);

    NT_ASSERT(context->txQueue == nullptr);
    NT_ASSERT(context->rxQueue == nullptr);

    auto macConfig = Read32(&context->regs->MacConfiguration);
    macConfig.ReceiverEnable = false;
    macConfig.TransmitterEnable = false;
    Write32(&context->regs->MacConfiguration, macConfig);

    TraceEntryExitWithStatus(DeviceD0Exit, LEVEL_INFO, status,
        TraceLoggingUInt32(targetState));
    return status;
}

static EVT_WDF_DEVICE_PREPARE_HARDWARE DevicePrepareHardware;
__declspec(code_seg("PAGE"))
static NTSTATUS
DevicePrepareHardware(
    _In_ WDFDEVICE device,
    _In_ WDFCMRESLIST resourcesRaw,
    _In_ WDFCMRESLIST resourcesTranslated)
{
    // PASSIVE_LEVEL
    PAGED_CODE();
    UNREFERENCED_PARAMETER(resourcesRaw);

    NTSTATUS status;
    PHYSICAL_ADDRESS maxPhysicalAddress;
    auto const context = DeviceGetContext(device);
    bool configHasMacAddress = false;

    // Read configuration

    {
        NETCONFIGURATION configuration;
        status = NetAdapterOpenConfiguration(context->adapter, WDF_NO_OBJECT_ATTRIBUTES, &configuration);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("NetAdapterOpenConfiguration-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }

        NET_ADAPTER_LINK_LAYER_ADDRESS configAddress;
        status = NetConfigurationQueryLinkLayerAddress(configuration, &configAddress);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("QueryLinkLayerAddress-not-found", LEVEL_VERBOSE,
                TraceLoggingNTStatus(status));
        }
        else if (configAddress.Length != ETHERNET_LENGTH_OF_ADDRESS)
        {
            TraceWrite("QueryLinkLayerAddress-bad-length", LEVEL_WARNING,
                TraceLoggingHexInt16(configAddress.Length, "Length"));
        }
        else if (
            ETH_IS_MULTICAST(configAddress.Address) ||
            ETH_IS_BROADCAST(configAddress.Address))
        {
            TraceWrite("QueryLinkLayerAddress-bad-address", LEVEL_WARNING,
                TraceLoggingBinary(configAddress.Address, ETHERNET_LENGTH_OF_ADDRESS, "address"));
        }
        else
        {
            TraceWrite("QueryLinkLayerAddress-found", LEVEL_INFO,
                TraceLoggingBinary(configAddress.Address, ETHERNET_LENGTH_OF_ADDRESS, "address"));
            memcpy(context->currentMacAddress, configAddress.Address, sizeof(context->currentMacAddress));
            configHasMacAddress = true;
        }
    }

    // Configure resources

    {
        NT_ASSERT(context->regs == nullptr);
        NT_ASSERT(context->interrupt == nullptr);

        unsigned interruptsFound = 0;
        ULONG const resourcesCount = WdfCmResourceListGetCount(resourcesTranslated);
        for (ULONG i = 0; i != resourcesCount; i += 1)
        {
            auto desc = WdfCmResourceListGetDescriptor(resourcesTranslated, i);
            switch (desc->Type)
            {
            case CmResourceTypeMemory:
                if (context->regs != nullptr)
                {
                    TraceWrite("DevicePrepareHardware-memory-unexpected", LEVEL_WARNING,
                        TraceLoggingHexInt64(desc->u.Memory.Start.QuadPart, "start"));
                }
                else if (desc->u.Memory.Length < sizeof(*context->regs))
                {
                    TraceWrite("DevicePrepareHardware-memory-small", LEVEL_WARNING,
                        TraceLoggingHexInt64(desc->u.Memory.Start.QuadPart, "start"),
                        TraceLoggingHexInt32(desc->u.Memory.Length, "length"));
                }
                else
                {
                    TraceWrite("DevicePrepareHardware-memory", LEVEL_VERBOSE,
                        TraceLoggingHexInt64(desc->u.Memory.Start.QuadPart, "start"),
                        TraceLoggingHexInt32(desc->u.Memory.Length, "length"));

                    context->regs = static_cast<MacRegisters*>(
                        MmMapIoSpaceEx(desc->u.Memory.Start, sizeof(*context->regs), PAGE_READWRITE | PAGE_NOCACHE));
                    if (context->regs == nullptr)
                    {
                        TraceWrite("MmMapIoSpaceEx-failed", LEVEL_ERROR);
                        status = STATUS_INSUFFICIENT_RESOURCES;
                        goto Done;
                    }
                }
                break;

            case CmResourceTypeInterrupt:
                switch (interruptsFound++)
                {
                case 0:
                    TraceWrite("DevicePrepareHardware-interrupt-sbd", LEVEL_VERBOSE,
                        TraceLoggingHexInt32(desc->u.Interrupt.Vector, "vector"));

                    WDF_INTERRUPT_CONFIG config;
                    WDF_INTERRUPT_CONFIG_INIT(&config, DeviceInterruptIsr, DeviceInterruptDpc);
                    config.InterruptRaw = WdfCmResourceListGetDescriptor(resourcesRaw, i);
                    config.InterruptTranslated = desc;

                    status = WdfInterruptCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES, &context->interrupt);
                    if (!NT_SUCCESS(status))
                    {
                        TraceWrite("WdfInterruptCreate-failed", LEVEL_ERROR,
                            TraceLoggingNTStatus(status));
                        goto Done;
                    }
                    break;
                case 1:
                    TraceWrite("DevicePrepareHardware-interrupt-pmt", LEVEL_VERBOSE,
                        TraceLoggingHexInt32(desc->u.Interrupt.Vector, "vector"));
                    break;
                default:
                    TraceWrite("DevicePrepareHardware-interrupt-unexpected", LEVEL_WARNING,
                        TraceLoggingHexInt32(desc->u.Interrupt.Vector, "vector"));
                    break;
                }
                break;

            default:
                TraceWrite("DevicePrepareHardware-resource-unexpected", LEVEL_WARNING,
                    TraceLoggingUInt8(desc->Type, "type"));
                break;
            }
        }
    }

    if (context->regs == nullptr)
    {
        TraceWrite("DevicePrepareHardware-no-memory", LEVEL_ERROR);
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Done;
    }

    if (context->interrupt == nullptr)
    {
        TraceWrite("DevicePrepareHardware-no-interrupt", LEVEL_ERROR);
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Done;
    }

    // Set up MAC address

    {
        auto const mac0hi = Read32(&context->regs->MacAddress[0].High);
        auto const mac0lo = Read32(&context->regs->MacAddress[0].Low);
        context->permanentMacAddress[0] = mac0lo.Addr0;
        context->permanentMacAddress[1] = mac0lo.Addr1;
        context->permanentMacAddress[2] = mac0lo.Addr2;
        context->permanentMacAddress[3] = mac0lo.Addr3;
        context->permanentMacAddress[4] = mac0hi.Addr4;
        context->permanentMacAddress[5] = mac0hi.Addr5;

        if (!configHasMacAddress)
        {
            memcpy(context->currentMacAddress, context->permanentMacAddress, sizeof(context->currentMacAddress));
            if ((mac0lo.Value32 == 0xFFFFFFFF && mac0hi.Value16Low == 0xFFFF) ||
                (mac0lo.Value32 == 0x00000000 && mac0hi.Value16Low == 0x0000))
            {
                TraceWrite("DevicePrepareHardware-Mac0-bad", LEVEL_WARNING,
                    TraceLoggingBinary(context->currentMacAddress, sizeof(context->currentMacAddress), "Mac0"));
                context->currentMacAddress[0] = 0xF2;
                context->currentMacAddress[1] = 0x00;
                BCryptGenRandom(
                    nullptr,
                    context->currentMacAddress + 2,
                    sizeof(context->currentMacAddress) - 2,
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            }
            else if (context->currentMacAddress[0] & 1u)
            {
                TraceWrite("DevicePrepareHardware-Mac0-fixup", LEVEL_WARNING,
                    TraceLoggingBinary(context->currentMacAddress, sizeof(context->currentMacAddress), "Mac0"));
                context->currentMacAddress[0] &= ~1u;
            }
        }
    }

    // Read features

    {
        auto const version = Read32(&context->regs->MacVersion);
        context->feature0 = Read32(&context->regs->MacHwFeature0);
        context->feature1 = Read32(&context->regs->MacHwFeature1);
        context->feature2 = Read32(&context->regs->MacHwFeature2);
        context->feature3 = Read32(&context->regs->MacHwFeature3);
        TraceWrite("DevicePrepareHardware-config", LEVEL_INFO,
            TraceLoggingHexInt32(version.RkVer, "RkVer"),
            TraceLoggingHexInt32(version.UserVer, "UserVer"),
            TraceLoggingHexInt32(context->feature0.Value32, "HwFeature0"),
            TraceLoggingHexInt32(context->feature1.Value32, "HwFeature1"),
            TraceLoggingHexInt32(context->feature2.Value32, "HwFeature2"),
            TraceLoggingHexInt32(context->feature3.Value32, "HwFeature3"),
            TraceLoggingBinary(context->permanentMacAddress, sizeof(context->permanentMacAddress), "PermanentAddr"),
            TraceLoggingBinary(context->currentMacAddress, sizeof(context->currentMacAddress), "CurrentAddr"));

        if (version.RkVer < 0x51 || version.UserVer > 0x52)
        {
            TraceWrite("DevicePrepareHardware-RkVer-not-supported", LEVEL_ERROR,
                TraceLoggingHexInt32(version.RkVer, "RkVer"));
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto Done;
        }
    }

    // Create DMA enabler

    {
        auto const profile = context->feature1.AddressWidth == AddressWidth_32
            ? WdfDmaProfileScatterGather
            : WdfDmaProfileScatterGather64;
        WDF_DMA_ENABLER_CONFIG config;
        WDF_DMA_ENABLER_CONFIG_INIT(&config, profile, 16384); // TODO: Jumbo packets.
        config.WdmDmaVersionOverride = 3;

        switch (context->feature1.AddressWidth)
        {
        case AddressWidth_32:
            config.AddressWidthOverride = 32;
            maxPhysicalAddress.QuadPart = 0xFFFFFFFF;
            break;
        case AddressWidth_40:
            config.AddressWidthOverride = 40;
            maxPhysicalAddress.QuadPart = 0xFFFFFFFFFF;
            break;
        case AddressWidth_48:
            config.AddressWidthOverride = 48;
            maxPhysicalAddress.QuadPart = 0xFFFFFFFFFFFF;
            break;
        default:
            TraceWrite("DevicePrepareHardware-AddressWidth-unknown", LEVEL_ERROR,
                TraceLoggingHexInt32(context->feature1.AddressWidth, "AddressWidth"));
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto Done;
        }

        status = WdfDmaEnablerCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES, &context->dma);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("WdfDmaEnablerCreate-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }
    }

    // Update adapter configuration.

    {
        NET_ADAPTER_LINK_LAYER_ADDRESS address;
        NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&address, sizeof(context->currentMacAddress), context->currentMacAddress);
        NetAdapterSetCurrentLinkLayerAddress(context->adapter, &address);
        NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&address, sizeof(context->permanentMacAddress), context->permanentMacAddress);
        NetAdapterSetPermanentLinkLayerAddress(context->adapter, &address);

        NET_ADAPTER_LINK_STATE linkState;
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&linkState);
        NetAdapterSetLinkState(context->adapter, &linkState);

        NET_ADAPTER_LINK_LAYER_CAPABILITIES linkCaps;
        auto const maxSpeed = context->feature0.Gmii ? 1'000'000'000u : 100'000'000u;
        NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&linkCaps, maxSpeed, maxSpeed);
        NetAdapterSetLinkLayerCapabilities(context->adapter, &linkCaps);

        NetAdapterSetLinkLayerMtuSize(context->adapter, 1500); // TODO: Jumbo packets.

        NET_ADAPTER_DMA_CAPABILITIES dmaCaps;
        NET_ADAPTER_DMA_CAPABILITIES_INIT(&dmaCaps, context->dma);
        dmaCaps.MaximumPhysicalAddress = maxPhysicalAddress;

        NET_ADAPTER_TX_CAPABILITIES txCaps;
        NET_ADAPTER_TX_CAPABILITIES_INIT_FOR_DMA(&txCaps, &dmaCaps, 1);
        txCaps.MaximumNumberOfFragments = QueueDescriptorMinCount - 1;

        NET_ADAPTER_RX_CAPABILITIES rxCaps; // TODO: Might use less memory if driver-managed.
        NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(&rxCaps, &dmaCaps, RxBufferSize, 1); // TODO: Jumbo packets.

        NetAdapterSetDataPathCapabilities(context->adapter, &txCaps, &rxCaps);

        // Note: If we don't claim support for everything, tcpip does not reliably bind.
        NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES rxFilterCaps;
        NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&rxFilterCaps, AdapterSetReceiveFilter);
        rxFilterCaps.MaximumMulticastAddresses =
            context->feature0.MacAddrCount > 1
            ? context->feature0.MacAddrCount - 1
            : 0;
        rxFilterCaps.SupportedPacketFilters =
            NetPacketFilterFlagDirected |
            (rxFilterCaps.MaximumMulticastAddresses ? NetPacketFilterFlagMulticast : NET_PACKET_FILTER_FLAGS()) |
            NetPacketFilterFlagAllMulticast |
            NetPacketFilterFlagBroadcast |
            NetPacketFilterFlagPromiscuous;
        NetAdapterSetReceiveFilterCapabilities(context->adapter, &rxFilterCaps);
    }

    // Initialize adapter.

    {
        status = DeviceReset(context->regs, context->currentMacAddress);
        if (!NT_SUCCESS(status))
        {
            goto Done;
        }

        // TODO: use ACPI _DSD?
        // TODO: review. This is what the NetBSD driver seems to be doing, and
        // it seems to work ok, but it doesn't line up with the documentation.
        auto busMode = Read32(&context->regs->DmaSysBusMode);
        busMode.Reserved14 = true; // mixed-burst?
        busMode.FixedBurst = false;
        busMode.AxiMaxWriteOutstanding = DefaultAxiMaxWriteOutstanding;
        busMode.AxiMaxReadOutstanding = DefaultAxiMaxReadOutstanding;
        busMode.BurstLength16 = true;
        busMode.BurstLength8 = true;
        busMode.BurstLength4 = true;
        Write32(&context->regs->DmaSysBusMode, busMode);

        Write32(&context->regs->Mac1usTicCounter, DefaultCsrRate / 1'000'000u - 1);

        static_assert(sizeof(RxDescriptor) == sizeof(TxDescriptor));
        static_assert(sizeof(RxDescriptor) % BusBytes == 0,
            "RxDescriptor must be a multiple of bus width.");
        ChannelDmaControl_t dmaControl = {};
        dmaControl.DescriptorSkipLength = (sizeof(RxDescriptor) - 16) / BusBytes;
        dmaControl.PblX8 = QueueBurstLengthX8;
        Write32(&context->regs->DmaCh[0].DmaControl, dmaControl);

        Write32(&context->regs->MmcControl, 0x1); // Reset counters.
    }

    // Start adapter.

    status = NetAdapterStart(context->adapter);
    if (!NT_SUCCESS(status))
    {
        TraceWrite("NetAdapterStart-failed", LEVEL_ERROR,
            TraceLoggingNTStatus(status));
        goto Done;
    }

Done:

    TraceEntryExitWithStatus(DevicePrepareHardware, LEVEL_INFO, status);
    return status;
}

static EVT_WDF_DEVICE_RELEASE_HARDWARE DeviceReleaseHardware;
__declspec(code_seg("PAGE"))
static NTSTATUS
DeviceReleaseHardware(
    _In_ WDFDEVICE device,
    _In_ WDFCMRESLIST resourcesTranslated)
{
    // PASSIVE_LEVEL
    PAGED_CODE();
    UNREFERENCED_PARAMETER(resourcesTranslated);

    auto const context = DeviceGetContext(device);
    if (context->regs != nullptr)
    {
#define CtxStat(x) TraceLoggingUInt32(context->x, #x)
#define RegStat(x) TraceLoggingUInt32(Read32(&context->regs->x), #x)

        TraceWrite("DeviceReleaseHardware-MacStats", LEVEL_INFO,
            CtxStat(isrHandled),
            CtxStat(isrIgnored),
            CtxStat(dpcLinkState),
            CtxStat(dpcRx),
            CtxStat(dpcTx),
            CtxStat(dpcAbnormalStatus),
            CtxStat(dpcFatalBusError));
        TraceWrite("DeviceReleaseHardware-TxStats", LEVEL_INFO,
            RegStat(TxPacketCountGoodBad),
            RegStat(TxUnderflowErrorPackets),
            RegStat(TxCarrierErrorPackets),
            RegStat(TxPacketCountGood),
            RegStat(TxPausePackets));
        TraceWrite("DeviceReleaseHardware-RxStats", LEVEL_INFO,
            RegStat(RxPacketCountGoodBad),
            RegStat(RxCrcErrorPackets),
            RegStat(RxLengthErrorPackets),
            RegStat(RxPausePackets),
            RegStat(RxFifoOverflowPackets),
            RegStat(RxWatchdogErrorPackets));

        DeviceReset(context->regs, context->permanentMacAddress);
        MmUnmapIoSpace(context->regs, sizeof(*context->regs));
        context->regs = nullptr;
    }

    TraceEntryExit(DeviceReleaseHardware, LEVEL_INFO);
    return STATUS_SUCCESS;
}

void
DeviceSetNotificationRxQueue(
    _In_ NETADAPTER adapter,
    _In_opt_ NETPACKETQUEUE rxQueue)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    auto const context = DeviceGetContext(AdapterGetContext(adapter)->device);

    WdfSpinLockAcquire(context->lock);

    context->rxQueue = rxQueue;
    if (rxQueue)
    {
        DeviceInterruptEnable_Locked(context, InterruptsRx);
    }
    else
    {
        DeviceInterruptDisable_Locked(context, InterruptsRx);
    }

    WdfSpinLockRelease(context->lock);
}

void
DeviceSetNotificationTxQueue(
    _In_ NETADAPTER adapter,
    _In_opt_ NETPACKETQUEUE txQueue)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    auto const context = DeviceGetContext(AdapterGetContext(adapter)->device);

    WdfSpinLockAcquire(context->lock);

    context->txQueue = txQueue;
    if (txQueue)
    {
        DeviceInterruptEnable_Locked(context, InterruptsTx);
    }
    else
    {
        DeviceInterruptDisable_Locked(context, InterruptsTx);
    }

    WdfSpinLockRelease(context->lock);
}

__declspec(code_seg("PAGE"))
NTSTATUS
DeviceAdd(
    _In_ WDFDRIVER driver,
    _Inout_ PWDFDEVICE_INIT deviceInit)
{
    // PASSIVE_LEVEL
    PAGED_CODE();
    UNREFERENCED_PARAMETER(driver);

    NTSTATUS status;

    // Configure deviceInit

    status = NetDeviceInitConfig(deviceInit);
    if (!NT_SUCCESS(status))
    {
        TraceWrite("NetDeviceInitConfig-failed", LEVEL_ERROR,
            TraceLoggingNTStatus(status));
        goto Done;
    }

    {
        WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
        pnpPowerCallbacks.EvtDevicePrepareHardware = DevicePrepareHardware;
        pnpPowerCallbacks.EvtDeviceReleaseHardware = DeviceReleaseHardware;
        pnpPowerCallbacks.EvtDeviceD0Entry = DeviceD0Entry;
        pnpPowerCallbacks.EvtDeviceD0Exit = DeviceD0Exit;
        WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpPowerCallbacks);
    }

    // Create device.

    WDFDEVICE device;
    {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);

        status = WdfDeviceCreate(&deviceInit, &attributes, &device);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("WdfDeviceCreate-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }

        WdfDeviceSetAlignmentRequirement(device, FILE_BYTE_ALIGNMENT);

        WDF_DEVICE_STATE deviceState;
        WDF_DEVICE_STATE_INIT(&deviceState);
        deviceState.NotDisableable = WdfFalse;
        WdfDeviceSetDeviceState(device, &deviceState);
    }

    // Create lock.

    {
        auto const context = DeviceGetContext(device);

        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device;

        status = WdfSpinLockCreate(&attributes, &context->lock);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("WdfSpinLockCreate-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }
    }

    // Create adapter.

    {
        auto const context = DeviceGetContext(device);
        auto const adapterInit = NetAdapterInitAllocate(device);
        if (adapterInit == nullptr)
        {
            TraceWrite("NetAdapterInitAllocate-failed", LEVEL_ERROR);
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Done;
        }

        NET_ADAPTER_DATAPATH_CALLBACKS callbacks;
        NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&callbacks, AdapterCreateTxQueue, AdapterCreateRxQueue);
        NetAdapterInitSetDatapathCallbacks(adapterInit, &callbacks);

        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AdapterContext);
        status = NetAdapterCreate(adapterInit, &attributes, &context->adapter);
        NetAdapterInitFree(adapterInit);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("NetAdapterCreate-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }

        auto const adapterContext = AdapterGetContext(context->adapter);
        NT_ASSERT(adapterContext->device == nullptr);
        adapterContext->device = device;
    }

Done:

    TraceEntryExitWithStatus(DeviceAdd, LEVEL_INFO, status);
    return status;
}
