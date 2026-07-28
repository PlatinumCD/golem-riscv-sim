#include "golem/runtime/deployment_runtime.h"

namespace golem::runtime {

namespace {

constexpr uint32_t kFrameMagic = UINT32_C(0x474f4c4d);
constexpr uintptr_t kDataAlignment = 64;
constexpr uint32_t kFrameHeaderWords = 5;
constexpr uint32_t kInvalidIndex = UINT32_MAX;

enum ReceivePhase : uint32_t {
    ReceiveMagic = 0,
    ReceiveRoute = 1,
    ReceiveExecutionLow = 2,
    ReceiveExecutionHigh = 3,
    ReceiveWordCount = 4,
    ReceivePayloadWords = 5,
    ReceiveDMASubmit = 6,
    ReceiveDMAActive = 7,
};

struct MemRefDescriptorHeader {
    void* allocated;
    void* aligned;
    int64_t offset;
};

static_assert(sizeof(MemRefDescriptorHeader) == 24);

extern "C" void* malloc(size_t size);

void zeroBytes(void* memory, size_t byte_count) {
    auto* bytes = static_cast<uint8_t*>(memory);
    for (size_t index = 0; index < byte_count; ++index) {
        bytes[index] = 0;
    }
}

void* allocateBytes(size_t byte_count) {
    if (byte_count == 0) {
        return nullptr;
    }
    void* memory = malloc(byte_count);
    if (memory != nullptr) {
        zeroBytes(memory, byte_count);
    }
    return memory;
}

uint8_t* alignPointer(void* allocation, uintptr_t alignment) {
    const uintptr_t raw = reinterpret_cast<uintptr_t>(allocation);
    const uintptr_t aligned = (raw + alignment - 1U) & ~(alignment - 1U);
    return reinterpret_cast<uint8_t*>(aligned);
}

void storeWord(uint8_t* bytes, uint32_t word) {
    bytes[0] = static_cast<uint8_t>(word);
    bytes[1] = static_cast<uint8_t>(word >> 8U);
    bytes[2] = static_cast<uint8_t>(word >> 16U);
    bytes[3] = static_cast<uint8_t>(word >> 24U);
}

uint64_t descriptorSize(uint32_t rank) {
    return sizeof(MemRefDescriptorHeader) +
           static_cast<uint64_t>(rank) * 2U * sizeof(int64_t);
}

}  // namespace

DeploymentRuntime::DeploymentRuntime(
    TileABI abi,
    RoutedWordTransport transport,
    DeploymentProfile* profile,
    DeploymentTrace* trace
) noexcept
    : abi_(abi),
      transport_(transport),
      execution_id_(0),
      error_(DeploymentError::None),
      initialized_(false),
      booted_(false),
      complete_task_count_(0),
      workspace_allocation_(nullptr),
      workspace_(nullptr),
      descriptor_allocation_(nullptr),
      descriptors_(nullptr),
      resource_tensors_(nullptr),
      resource_bound_(nullptr),
      resource_ready_(nullptr),
      task_complete_(nullptr),
      outgoing_route_sent_(nullptr),
      incoming_route_received_(nullptr),
      task_inputs_(nullptr),
      task_outputs_(nullptr),
      receive_states_(nullptr),
      receive_state_count_(0),
      transmit_active_(false),
      transmit_task_id_(0),
      transmit_route_index_(0),
      transmit_phase_(0),
      transmit_word_offset_(0),
      profile_(profile),
      trace_(trace) {
}

bool DeploymentRuntime::initialize() noexcept {
    if (initialized_) {
        return !failed();
    }
    if (!abi_.valid() || !abi_.hasDeploymentPlan()) {
        setError(DeploymentError::InvalidABI);
        return false;
    }
    if ((abi_.incoming_route_count != 0 ||
         abi_.outgoing_route_count != 0) &&
        !transport_.valid()) {
        setError(DeploymentError::InvalidTransport);
        return false;
    }
    if (!allocateState() || !initializeResources()) {
        if (!failed()) {
            setError(DeploymentError::AllocationFailed);
        }
        return false;
    }
    initialized_ = true;
    return true;
}

bool DeploymentRuntime::allocateState() noexcept {
    uint64_t descriptor_bytes = 0;
    uint32_t max_inputs = 0;
    uint32_t max_outputs = 0;
    for (uint32_t slot = 0; slot < abi_.resource_count; ++slot) {
        const uint64_t size = descriptorSize(abi_.resources[slot].rank);
        if (descriptor_bytes > UINT64_MAX - size) {
            return false;
        }
        descriptor_bytes += size;
    }
    for (uint32_t index = 0; index < abi_.task_binding_count; ++index) {
        const TaskBinding& binding = abi_.task_bindings[index];
        if (binding.input_count > max_inputs) {
            max_inputs = binding.input_count;
        }
        if (binding.output_count > max_outputs) {
            max_outputs = binding.output_count;
        }
    }
    if (descriptor_bytes > SIZE_MAX ||
        abi_.workspace_size > SIZE_MAX - (kDataAlignment - 1U)) {
        return false;
    }

    if (abi_.workspace_size != 0) {
        workspace_allocation_ = allocateBytes(
            static_cast<size_t>(abi_.workspace_size + kDataAlignment - 1U)
        );
        if (workspace_allocation_ == nullptr) {
            return false;
        }
        workspace_ = alignPointer(workspace_allocation_, kDataAlignment);
    }
    descriptor_allocation_ =
        allocateBytes(static_cast<size_t>(descriptor_bytes));
    descriptors_ = static_cast<uint8_t*>(descriptor_allocation_);
    resource_tensors_ = static_cast<Tensor*>(
        allocateBytes(sizeof(Tensor) * abi_.resource_count)
    );
    resource_bound_ = static_cast<bool*>(
        allocateBytes(sizeof(bool) * abi_.resource_count)
    );
    resource_ready_ = static_cast<bool*>(
        allocateBytes(sizeof(bool) * abi_.resource_count)
    );
    task_complete_ = static_cast<bool*>(
        allocateBytes(sizeof(bool) * abi_.task_binding_count)
    );
    outgoing_route_sent_ = static_cast<bool*>(
        allocateBytes(sizeof(bool) * abi_.outgoing_route_count)
    );
    incoming_route_received_ = static_cast<bool*>(
        allocateBytes(sizeof(bool) * abi_.incoming_route_count)
    );
    task_inputs_ = static_cast<Tensor*>(
        allocateBytes(sizeof(Tensor) * max_inputs)
    );
    task_outputs_ = static_cast<Tensor*>(
        allocateBytes(sizeof(Tensor) * max_outputs)
    );
    receive_states_ = static_cast<ReceiveState*>(
        allocateBytes(sizeof(ReceiveState) * abi_.incoming_route_count)
    );

    if ((descriptor_bytes != 0 && descriptors_ == nullptr) ||
        (abi_.resource_count != 0 &&
         (resource_tensors_ == nullptr ||
          resource_bound_ == nullptr ||
          resource_ready_ == nullptr)) ||
        (abi_.task_binding_count != 0 && task_complete_ == nullptr) ||
        (abi_.outgoing_route_count != 0 &&
         outgoing_route_sent_ == nullptr) ||
        (abi_.incoming_route_count != 0 &&
         (incoming_route_received_ == nullptr ||
          receive_states_ == nullptr)) ||
        (max_inputs != 0 && task_inputs_ == nullptr) ||
        (max_outputs != 0 && task_outputs_ == nullptr)) {
        return false;
    }

    for (uint32_t route_index = 0;
         route_index < abi_.incoming_route_count;
         ++route_index) {
        const uint32_t source = abi_.incoming_routes[route_index].source_core;
        bool known = false;
        for (uint32_t state_index = 0;
             state_index < receive_state_count_;
             ++state_index) {
            if (receive_states_[state_index].source_tile == source) {
                known = true;
                break;
            }
        }
        if (!known) {
            receive_states_[receive_state_count_].source_tile = source;
            ++receive_state_count_;
        }
    }
    return true;
}

bool DeploymentRuntime::initializeResources() noexcept {
    uint8_t* descriptor = descriptors_;
    for (uint32_t slot = 0; slot < abi_.resource_count; ++slot) {
        const Resource& resource = abi_.resources[slot];
        resource_tensors_[slot] = {
            resource.element_type,
            static_cast<int64_t>(resource.rank),
            descriptor,
        };
        descriptor += descriptorSize(resource.rank);

        if ((resource.flags & ResourceWorkspace) != 0) {
            if (!setResourceData(
                    slot,
                    workspace_ + resource.workspace_offset
                )) {
                return false;
            }
        }
    }
    return true;
}

bool DeploymentRuntime::setResourceData(
    uint32_t local_slot,
    void* data
) noexcept {
    const Resource* resource = abi_.findResource(local_slot);
    if (resource == nullptr || data == nullptr) {
        return false;
    }

    Tensor& tensor = resource_tensors_[local_slot];
    auto* header = static_cast<MemRefDescriptorHeader*>(tensor.descriptor);
    header->allocated = data;
    header->aligned = data;
    header->offset = 0;

    auto* sizes = reinterpret_cast<int64_t*>(
        static_cast<uint8_t*>(tensor.descriptor) +
        sizeof(MemRefDescriptorHeader)
    );
    auto* strides = sizes + resource->rank;
    uint64_t stride = 1;
    for (uint32_t reverse = 0; reverse < resource->rank; ++reverse) {
        const uint32_t dimension = resource->rank - reverse - 1U;
        const int64_t size =
            abi_.resource_dimensions[
                resource->dimension_offset + dimension
            ];
        sizes[dimension] = size;
        strides[dimension] = static_cast<int64_t>(stride);
        stride *= static_cast<uint64_t>(size);
    }
    resource_bound_[local_slot] = true;
    return true;
}

bool DeploymentRuntime::boot() noexcept {
    if (booted_) {
        return !failed();
    }
    if (!initialize()) {
        return false;
    }
    for (uint32_t index = 0; index < abi_.boot_task_count; ++index) {
        if (abi_.boot_tasks[index].execute(nullptr, 0, nullptr, 0) !=
            TaskStatus::Success) {
            setError(DeploymentError::BootTaskFailed);
            return false;
        }
    }
    booted_ = true;
    return true;
}

bool DeploymentRuntime::bindModelInput(
    uint32_t model_index,
    void* data
) noexcept {
    if (!initialize()) {
        return false;
    }
    for (uint32_t index = 0; index < abi_.model_input_count; ++index) {
        const ModelIO& input = abi_.model_inputs[index];
        if (input.model_index == model_index) {
            if (!setResourceData(input.local_slot, data)) {
                setError(DeploymentError::InvalidModelIO);
                return false;
            }
            resource_ready_[input.local_slot] = true;
            return true;
        }
    }
    setError(DeploymentError::InvalidModelIO);
    return false;
}

bool DeploymentRuntime::bindModelOutput(
    uint32_t model_index,
    void* data
) noexcept {
    if (!initialize()) {
        return false;
    }
    for (uint32_t index = 0; index < abi_.model_output_count; ++index) {
        const ModelIO& output = abi_.model_outputs[index];
        if (output.model_index == model_index) {
            if (!setResourceData(output.local_slot, data)) {
                setError(DeploymentError::InvalidModelIO);
                return false;
            }
            return true;
        }
    }
    setError(DeploymentError::InvalidModelIO);
    return false;
}

DeploymentStep DeploymentRuntime::step() noexcept {
#if defined(GOLEM_RUNTIME_ENABLE_PROFILE)
    const uint64_t start_cycle = profileCycle();
#define GOLEM_RECORD_PROFILE_STEP(activity) \
    recordProfileStep((activity), start_cycle)
#else
#define GOLEM_RECORD_PROFILE_STEP(activity) ((void)0)
#endif
    if (failed()) {
        GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Failed);
        return DeploymentStep::Failed;
    }
    if (!boot()) {
        GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Failed);
        return DeploymentStep::Failed;
    }
    if (complete()) {
        GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Complete);
        return DeploymentStep::Complete;
    }

    if (transport_.receiveDMAAvailable()) {
        const bool progressed = progressReceiveDMA();
        if (failed()) {
            GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Failed);
            return DeploymentStep::Failed;
        }
        if (progressed) {
            const DeploymentStep result =
                complete() ? DeploymentStep::Complete
                           : DeploymentStep::Progress;
            GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Receive);
            return result;
        }
    }

    RoutedWord word{};
    if (transport_.tryReceive(&word)) {
        if (!consumeWord(word)) {
            if (!failed()) {
                setError(DeploymentError::InvalidFrame);
            }
            GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Failed);
            return DeploymentStep::Failed;
        }
        const DeploymentStep result =
            complete() ? DeploymentStep::Complete
                       : DeploymentStep::Progress;
        GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Receive);
        return result;
    }

    if (transmit_active_) {
        const bool progressed = progressTaskRoutes();
        if (failed()) {
            GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Failed);
            return DeploymentStep::Failed;
        }
        if (complete()) {
            GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Transmit);
            return DeploymentStep::Complete;
        }
        const DeploymentStep result =
            progressed ? DeploymentStep::Progress
                       : DeploymentStep::Idle;
        GOLEM_RECORD_PROFILE_STEP(
            progressed
                ? ProfileActivity::Transmit
                : ProfileActivity::TransmitBlocked);
        return result;
    }

    if (executeReadyTask()) {
        const DeploymentStep result =
            complete() ? DeploymentStep::Complete
                       : DeploymentStep::Progress;
        GOLEM_RECORD_PROFILE_STEP(ProfileActivity::Execute);
        return result;
    }
    const bool receive_wait =
        !failed() && hasPendingIncomingRoute();
    const DeploymentStep result =
        failed()
            ? DeploymentStep::Failed
            : (receive_wait
                   ? DeploymentStep::WaitForReceive
                   : DeploymentStep::Idle);
    GOLEM_RECORD_PROFILE_STEP(
        failed()
            ? ProfileActivity::Failed
            : (receive_wait
                   ? ProfileActivity::ReceiveWait
                   : ProfileActivity::Idle));
#undef GOLEM_RECORD_PROFILE_STEP
    return result;
}

bool DeploymentRuntime::taskReady(uint32_t task_index) const noexcept {
    if (task_index >= abi_.task_binding_count ||
        task_complete_[task_index]) {
        return false;
    }
    const TaskBinding& binding = abi_.task_bindings[task_index];
    for (uint32_t input = 0; input < binding.input_count; ++input) {
        const uint32_t slot =
            abi_.task_binding_data[binding.input_offset + input];
        if (!resource_bound_[slot] || !resource_ready_[slot]) {
            return false;
        }
    }
    for (uint32_t output = 0; output < binding.output_count; ++output) {
        const uint32_t slot =
            abi_.task_binding_data[binding.output_offset + output];
        if (!resource_bound_[slot]) {
            return false;
        }
    }
    for (uint32_t dependency = 0;
         dependency < binding.dependency_count;
         ++dependency) {
        const uint32_t dependency_id =
            abi_.task_binding_data[
                binding.dependency_offset + dependency
            ];
        const uint32_t dependency_index = taskIndex(dependency_id);
        if (dependency_index == kInvalidIndex ||
            !task_complete_[dependency_index]) {
            return false;
        }
    }
    return true;
}

bool DeploymentRuntime::executeReadyTask() noexcept {
    for (uint32_t task_index = 0;
         task_index < abi_.task_binding_count;
         ++task_index) {
        if (!taskReady(task_index)) {
            continue;
        }
        const TaskBinding& binding = abi_.task_bindings[task_index];
        const Task& task = abi_.dispatch_tasks.tasks[task_index];
        for (uint32_t input = 0; input < binding.input_count; ++input) {
            const uint32_t slot =
                abi_.task_binding_data[binding.input_offset + input];
            task_inputs_[input] = resource_tensors_[slot];
        }
        for (uint32_t output = 0; output < binding.output_count; ++output) {
            const uint32_t slot =
                abi_.task_binding_data[binding.output_offset + output];
            task_outputs_[output] = resource_tensors_[slot];
        }
        emitTaskTrace(TaskTraceEvent::Start, task.id);
        const TaskStatus status = task.execute(
            task_inputs_,
            binding.input_count,
            task_outputs_,
            binding.output_count
        );
        emitTaskTrace(TaskTraceEvent::Finish, task.id);
        if (status != TaskStatus::Success) {
            setError(DeploymentError::TaskFailed);
            return false;
        }
        for (uint32_t output = 0; output < binding.output_count; ++output) {
            const uint32_t slot =
                abi_.task_binding_data[binding.output_offset + output];
            resource_ready_[slot] = true;
        }
        task_complete_[task_index] = true;
        ++complete_task_count_;
        if (!beginTaskRoutes(task.id)) {
            setError(DeploymentError::RouteSendFailed);
            return false;
        }
        return true;
    }
    return false;
}

bool DeploymentRuntime::beginTaskRoutes(uint32_t task_id) noexcept {
    if (transmit_active_) {
        return false;
    }
    transmit_task_id_ = task_id;
    transmit_route_index_ = 0;
    transmit_phase_ = 0;
    transmit_word_offset_ = 0;
    transmit_active_ = true;

    while (transmit_route_index_ < abi_.outgoing_route_count) {
        const Route& route =
            abi_.outgoing_routes[transmit_route_index_];
        if (!outgoing_route_sent_[transmit_route_index_] &&
            route.source_task == transmit_task_id_) {
            return true;
        }
        ++transmit_route_index_;
    }
    transmit_active_ = false;
    return true;
}

bool DeploymentRuntime::progressTaskRoutes() noexcept {
    if (!transmit_active_) {
        return false;
    }

    while (transmit_route_index_ < abi_.outgoing_route_count) {
        const Route& candidate =
            abi_.outgoing_routes[transmit_route_index_];
        if (!outgoing_route_sent_[transmit_route_index_] &&
            candidate.source_task == transmit_task_id_) {
            break;
        }
        ++transmit_route_index_;
    }

    if (transmit_route_index_ == abi_.outgoing_route_count) {
        transmit_active_ = false;
        return true;
    }

    const Route& route =
        abi_.outgoing_routes[transmit_route_index_];
    const Resource* resource = abi_.findResource(route.local_slot);
    if (resource == nullptr ||
        !resource_ready_[route.local_slot] ||
        route.byte_size == 0 ||
        route.byte_size % sizeof(uint32_t) != 0 ||
        route.byte_size / sizeof(uint32_t) > UINT32_MAX) {
        setError(DeploymentError::RouteSendFailed);
        transmit_active_ = false;
        return false;
    }
    const auto* descriptor = static_cast<MemRefDescriptorHeader*>(
        resource_tensors_[route.local_slot].descriptor
    );
    if (descriptor == nullptr || descriptor->aligned == nullptr) {
        setError(DeploymentError::RouteSendFailed);
        transmit_active_ = false;
        return false;
    }

    const uint32_t atomic_limit =
        transport_.try_send_words == nullptr
            ? 1U
            : kTransportBurstWords;

    if (transmit_phase_ == 0) {
        const uint32_t header[kFrameHeaderWords] = {
            kFrameMagic,
            route.id,
            static_cast<uint32_t>(execution_id_),
            static_cast<uint32_t>(execution_id_ >> 32U),
            static_cast<uint32_t>(
                route.byte_size / sizeof(uint32_t)
            ),
        };
        const uint32_t remaining =
            kFrameHeaderWords -
            static_cast<uint32_t>(transmit_word_offset_);
        const uint32_t send_words =
            remaining < atomic_limit ? remaining : atomic_limit;
        if (!transport_.trySendWords(
            route.destination_core,
            header + transmit_word_offset_,
            send_words
        )) {
            return false;
        }
        transmit_word_offset_ += send_words;
        if (transmit_word_offset_ == kFrameHeaderWords) {
            transmit_phase_ = 1;
            transmit_word_offset_ = 0;
        }
        return true;
    }

    const auto* words =
        static_cast<const uint32_t*>(descriptor->aligned);
    const uint64_t word_count =
        route.byte_size / sizeof(uint32_t);
    const uint64_t remaining = word_count - transmit_word_offset_;
    const uint32_t send_words = static_cast<uint32_t>(
        remaining < atomic_limit ? remaining : atomic_limit
    );
    if (!transport_.trySendWords(
        route.destination_core,
        words + transmit_word_offset_,
        send_words
    )) {
        return false;
    }
    transmit_word_offset_ += send_words;
    if (transmit_word_offset_ == word_count) {
        outgoing_route_sent_[transmit_route_index_] = true;
        ++transmit_route_index_;
        transmit_phase_ = 0;
        transmit_word_offset_ = 0;
    }
    return true;
}

bool DeploymentRuntime::allOutgoingRoutesSent() const noexcept {
    for (uint32_t index = 0;
         index < abi_.outgoing_route_count;
         ++index) {
        if (!outgoing_route_sent_[index]) {
            return false;
        }
    }
    return true;
}

bool DeploymentRuntime::hasPendingIncomingRoute() const noexcept {
    for (uint32_t index = 0;
         index < abi_.incoming_route_count;
         ++index) {
        if (!incoming_route_received_[index]) {
            return true;
        }
    }
    return false;
}

DeploymentRuntime::ReceiveState* DeploymentRuntime::receiveState(
    uint32_t source_tile
) noexcept {
    for (uint32_t index = 0; index < receive_state_count_; ++index) {
        if (receive_states_[index].source_tile == source_tile) {
            return &receive_states_[index];
        }
    }
    return nullptr;
}

bool DeploymentRuntime::progressReceiveDMA() noexcept {
    if (!transport_.receiveDMAAvailable()) {
        return false;
    }

    uint32_t completed_source = 0;
    uint32_t completed_route = 0;
    if (transport_.tryReceiveWordsCompletion(
            &completed_source, &completed_route)) {
        ReceiveState* state = receiveState(completed_source);
        if (state == nullptr ||
            state->phase != ReceiveDMAActive ||
            state->route_id != completed_route) {
            setError(DeploymentError::InvalidFrame);
            return true;
        }
        const Route* route =
            abi_.findIncomingRoute(completed_route);
        if (route == nullptr ||
            route->source_core != completed_source) {
            setError(DeploymentError::InvalidFrame);
            return true;
        }
        uint32_t route_index = 0;
        while (&abi_.incoming_routes[route_index] != route) {
            ++route_index;
        }
        if (incoming_route_received_[route_index]) {
            setError(DeploymentError::InvalidFrame);
            return true;
        }
        incoming_route_received_[route_index] = true;
        resource_ready_[route->local_slot] = true;
        state->phase = ReceiveMagic;
        state->route_id = 0;
        state->execution_id = 0;
        state->word_count = 0;
        state->received_words = 0;
        state->destination = nullptr;
        return true;
    }

    for (uint32_t index = 0;
         index < receive_state_count_;
         ++index) {
        ReceiveState& state = receive_states_[index];
        if (state.phase != ReceiveDMASubmit) {
            continue;
        }
        if (!transport_.tryStartReceiveWords(
                state.source_tile,
                state.route_id,
                state.destination,
                state.word_count)) {
            return false;
        }
        state.phase = ReceiveDMAActive;
        return true;
    }
    return false;
}

bool DeploymentRuntime::consumeWord(const RoutedWord& word) noexcept {
    ReceiveState* state = receiveState(word.source_tile);
    if (state == nullptr) {
        return false;
    }
    switch (state->phase) {
        case ReceiveMagic:
            if (word.payload != kFrameMagic) {
                return false;
            }
            state->phase = ReceiveRoute;
            return true;
        case ReceiveRoute:
            state->route_id = word.payload;
            state->phase = ReceiveExecutionLow;
            return true;
        case ReceiveExecutionLow:
            state->execution_id = word.payload;
            state->phase = ReceiveExecutionHigh;
            return true;
        case ReceiveExecutionHigh:
            state->execution_id |=
                static_cast<ExecutionId>(word.payload) << 32U;
            state->phase = ReceiveWordCount;
            return true;
        case ReceiveWordCount: {
            const Route* route = abi_.findIncomingRoute(state->route_id);
            if (route == nullptr ||
                route->source_core != word.source_tile ||
                state->execution_id != execution_id_ ||
                route->byte_size / sizeof(uint32_t) != word.payload) {
                return false;
            }
            uint32_t route_index = 0;
            while (&abi_.incoming_routes[route_index] != route) {
                ++route_index;
            }
            if (incoming_route_received_[route_index]) {
                return false;
            }
            auto* descriptor = static_cast<MemRefDescriptorHeader*>(
                resource_tensors_[route->local_slot].descriptor
            );
            state->word_count = word.payload;
            state->received_words = 0;
            state->destination = static_cast<uint8_t*>(descriptor->aligned);
            state->phase =
                transport_.receiveDMAAvailable()
                    ? ReceiveDMASubmit
                    : ReceivePayloadWords;
            return state->word_count != 0;
        }
        case ReceivePayloadWords: {
            storeWord(
                state->destination +
                    static_cast<uint64_t>(state->received_words) *
                        sizeof(uint32_t),
                word.payload
            );
            ++state->received_words;
            if (state->received_words == state->word_count) {
                const Route* route =
                    abi_.findIncomingRoute(state->route_id);
                if (route == nullptr) {
                    return false;
                }
                uint32_t route_index = 0;
                while (&abi_.incoming_routes[route_index] != route) {
                    ++route_index;
                }
                incoming_route_received_[route_index] = true;
                resource_ready_[route->local_slot] = true;
                state->phase = ReceiveMagic;
            }
            return true;
        }
        case ReceiveDMASubmit:
        case ReceiveDMAActive:
            return false;
        default:
            return false;
    }
}

uint32_t DeploymentRuntime::taskIndex(uint32_t task_id) const noexcept {
    for (uint32_t index = 0; index < abi_.task_binding_count; ++index) {
        if (abi_.task_bindings[index].task_id == task_id) {
            return index;
        }
    }
    return kInvalidIndex;
}

bool DeploymentRuntime::complete() const noexcept {
    return initialized_ &&
           !failed() &&
           complete_task_count_ == abi_.task_binding_count &&
           !transmit_active_ &&
           allOutgoingRoutesSent();
}

bool DeploymentRuntime::failed() const noexcept {
    return error_ != DeploymentError::None;
}

DeploymentError DeploymentRuntime::error() const noexcept {
    return error_;
}

ExecutionId DeploymentRuntime::executionId() const noexcept {
    return execution_id_;
}

const Tensor* DeploymentRuntime::resourceTensor(
    uint32_t local_slot
) const noexcept {
    return initialized_ && local_slot < abi_.resource_count
               ? &resource_tensors_[local_slot]
               : nullptr;
}

const TileABI& DeploymentRuntime::abi() const noexcept {
    return abi_;
}

void DeploymentRuntime::setError(DeploymentError error) noexcept {
    if (error_ == DeploymentError::None) {
        error_ = error;
    }
}

uint64_t DeploymentRuntime::profileCycle() const noexcept {
    return profile_ != nullptr && profile_->read_cycle != nullptr
               ? profile_->read_cycle(profile_->context)
               : 0;
}

void DeploymentRuntime::recordProfileStep(
    ProfileActivity activity,
    uint64_t start_cycle
) noexcept {
    if (profile_ == nullptr || profile_->read_cycle == nullptr) {
        return;
    }
    const uint64_t cycles =
        profile_->read_cycle(profile_->context) - start_cycle;
    switch (activity) {
        case ProfileActivity::Receive:
            ++profile_->receive_steps;
            profile_->receive_cycles += cycles;
            return;
        case ProfileActivity::Transmit:
            ++profile_->transmit_steps;
            profile_->transmit_cycles += cycles;
            return;
        case ProfileActivity::TransmitBlocked:
            ++profile_->transmit_blocked_steps;
            profile_->transmit_blocked_cycles += cycles;
            return;
        case ProfileActivity::Execute:
            ++profile_->execute_steps;
            profile_->execute_cycles += cycles;
            return;
        case ProfileActivity::Idle:
            ++profile_->idle_steps;
            profile_->idle_cycles += cycles;
            return;
        case ProfileActivity::ReceiveWait:
            ++profile_->receive_wait_steps;
            profile_->receive_wait_cycles += cycles;
            return;
        case ProfileActivity::Complete:
            ++profile_->complete_steps;
            profile_->complete_cycles += cycles;
            return;
        case ProfileActivity::Failed:
            ++profile_->failed_steps;
            profile_->failed_cycles += cycles;
            return;
    }
}

void DeploymentRuntime::emitTaskTrace(
    TaskTraceEvent event,
    uint32_t task_id
) noexcept {
    if (trace_ != nullptr && trace_->emit != nullptr) {
        trace_->emit(
            trace_->context,
            event,
            task_id,
            execution_id_
        );
    }
}

}  // namespace golem::runtime
