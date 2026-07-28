#include "delta_kernel.hpp"

#ifndef DELTA_KERNEL_FFI_HEADER
#define DELTA_KERNEL_FFI_HEADER "delta_kernel_ffi.hpp"
#endif
#include DELTA_KERNEL_FFI_HEADER

#include <cstring>
#include <type_traits>

namespace delta {

namespace {

[[noreturn]] void Throw(const char *operation, char *error) {
	std::string message = error ? std::string(error) : std::string("unknown error");
	if (error) {
		ffi::delta_string_free(error);
	}
	throw DeltaError(std::string(operation) + " failed: " + message);
}

template <class Message>
Message Decode(const char *operation, uint8_t *bytes, size_t length, char *error) {
	if (!bytes) {
		Throw(operation, error);
	}
	Message message;
	const bool parsed = message.ParseFromArray(bytes, static_cast<int>(length));
	ffi::delta_bytes_free(bytes, length);
	if (!parsed) {
		throw DeltaError(std::string(operation) + ": invalid protobuf payload");
	}
	return message;
}

struct VisitorError : ffi::EngineError {
	std::string message;
};

ffi::EngineError *AllocateVisitorError(ffi::KernelError type, ffi::KernelStringSlice message) {
	auto *error = new VisitorError;
	error->etype = type;
	error->message.assign(message.ptr, message.len);
	return error;
}

template <class T>
bool Unpack(ffi::ExternResult<T> result, T &value, std::string &error) {
	if (result.tag == ffi::ExternResult<T>::Tag::Ok) {
		value = result.ok._0;
		return true;
	}
	auto *raw = result.err._0;
	if (raw) {
		auto *visitor_error = static_cast<VisitorError *>(raw);
		error = visitor_error->message;
		delete visitor_error;
	} else {
		error = "kernel expression visitor returned an unknown error";
	}
	return false;
}

ffi::KernelStringSlice Slice(const std::string &value) {
	return {value.data(), value.size()};
}

} // namespace

//===----------------------------------------------------------------------===//
// Arrow ownership
//===----------------------------------------------------------------------===//

ArrowBatch::~ArrowBatch() {
	reset();
}

ArrowBatch::ArrowBatch(ArrowBatch &&other) noexcept : array_(other.array_), schema_(other.schema_) {
	other.array_ = {};
	other.schema_ = {};
}

ArrowBatch &ArrowBatch::operator=(ArrowBatch &&other) noexcept {
	if (this != &other) {
		reset();
		array_ = other.array_;
		schema_ = other.schema_;
		other.array_ = {};
		other.schema_ = {};
	}
	return *this;
}

void ArrowBatch::reset() noexcept {
	if (array_.release) {
		array_.release(&array_);
	}
	if (schema_.release) {
		schema_.release(&schema_);
	}
	array_ = {};
	schema_ = {};
}

//===----------------------------------------------------------------------===//
// Predicate value model and C visitor bridge
//===----------------------------------------------------------------------===//

struct Expression::Impl {
	enum class Kind { Column, Bool, I8, I16, I32, I64, F32, F64, String, Date, Decimal };
	Kind kind;
	std::string string_value;
	uint64_t high = 0;
	uint64_t low = 0;
	double floating = 0;
	int64_t integer = 0;
	uint8_t precision = 0;
	uint8_t scale = 0;
};

struct Predicate::Impl {
	enum class Kind { Compare, And, Or, Not, IsNull };
	Kind kind;
	Comparison comparison = Comparison::Equal;
	std::optional<Expression> left;
	std::optional<Expression> right;
	std::vector<Predicate> children;
};

Expression Expression::column(std::string name) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::Column;
	impl->string_value = std::move(name);
	return Expression(std::move(impl));
}
Expression Expression::boolean(bool value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::Bool;
	impl->integer = value;
	return Expression(std::move(impl));
}
Expression Expression::int8(int8_t value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::I8;
	impl->integer = value;
	return Expression(std::move(impl));
}
Expression Expression::int16(int16_t value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::I16;
	impl->integer = value;
	return Expression(std::move(impl));
}
Expression Expression::int32(int32_t value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::I32;
	impl->integer = value;
	return Expression(std::move(impl));
}
Expression Expression::int64(int64_t value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::I64;
	impl->integer = value;
	return Expression(std::move(impl));
}
Expression Expression::float32(float value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::F32;
	impl->floating = value;
	return Expression(std::move(impl));
}
Expression Expression::float64(double value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::F64;
	impl->floating = value;
	return Expression(std::move(impl));
}
Expression Expression::string(std::string value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::String;
	impl->string_value = std::move(value);
	return Expression(std::move(impl));
}
Expression Expression::date(int32_t value) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::Date;
	impl->integer = value;
	return Expression(std::move(impl));
}
Expression Expression::decimal(uint64_t high, uint64_t low, uint8_t precision, uint8_t scale) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::Decimal;
	impl->high = high;
	impl->low = low;
	impl->precision = precision;
	impl->scale = scale;
	return Expression(std::move(impl));
}

Predicate Predicate::compare(Comparison comparison, Expression left, Expression right) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::Compare;
	impl->comparison = comparison;
	impl->left = std::move(left);
	impl->right = std::move(right);
	return Predicate(std::move(impl));
}
Predicate Predicate::all(std::vector<Predicate> children) {
	if (children.empty()) {
		throw DeltaError("Predicate::all requires at least one child");
	}
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::And;
	impl->children = std::move(children);
	return Predicate(std::move(impl));
}
Predicate Predicate::any(std::vector<Predicate> children) {
	if (children.empty()) {
		throw DeltaError("Predicate::any requires at least one child");
	}
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::Or;
	impl->children = std::move(children);
	return Predicate(std::move(impl));
}
Predicate Predicate::logical_not(Predicate child) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::Not;
	impl->children.push_back(std::move(child));
	return Predicate(std::move(impl));
}
Predicate Predicate::is_null(Expression expression) {
	auto impl = std::make_shared<Impl>();
	impl->kind = Impl::Kind::IsNull;
	impl->left = std::move(expression);
	return Predicate(std::move(impl));
}

struct PredicateBridge {
	explicit PredicateBridge(const Predicate &predicate) : root(predicate) {
		ffi.predicate = this;
		ffi.visitor = &VisitRoot;
	}

	static uintptr_t VisitRoot(void *self, ffi::KernelExpressionVisitorState *state) noexcept {
		auto &bridge = *static_cast<PredicateBridge *>(self);
		return bridge.VisitPredicate(bridge.root, state);
	}

	uintptr_t VisitExpression(const Expression &expression, ffi::KernelExpressionVisitorState *state) noexcept {
		const auto &impl = *expression.impl_;
		uintptr_t result = ~uintptr_t(0);
		switch (impl.kind) {
		case Expression::Impl::Kind::Column: {
			auto value = ffi::visit_expression_column(state, Slice(impl.string_value), AllocateVisitorError);
			if (!Unpack(value, result, error)) {
				return ~uintptr_t(0);
			}
			return result;
		}
		case Expression::Impl::Kind::Bool:
			return ffi::visit_expression_literal_bool(state, impl.integer != 0);
		case Expression::Impl::Kind::I8:
			return ffi::visit_expression_literal_byte(state, static_cast<int8_t>(impl.integer));
		case Expression::Impl::Kind::I16:
			return ffi::visit_expression_literal_short(state, static_cast<int16_t>(impl.integer));
		case Expression::Impl::Kind::I32:
			return ffi::visit_expression_literal_int(state, static_cast<int32_t>(impl.integer));
		case Expression::Impl::Kind::I64:
			return ffi::visit_expression_literal_long(state, impl.integer);
		case Expression::Impl::Kind::F32:
			return ffi::visit_expression_literal_float(state, static_cast<float>(impl.floating));
		case Expression::Impl::Kind::F64:
			return ffi::visit_expression_literal_double(state, impl.floating);
		case Expression::Impl::Kind::String: {
			auto value = ffi::visit_expression_literal_string(state, Slice(impl.string_value), AllocateVisitorError);
			if (!Unpack(value, result, error)) {
				return ~uintptr_t(0);
			}
			return result;
		}
		case Expression::Impl::Kind::Date:
			return ffi::visit_expression_literal_date(state, static_cast<int32_t>(impl.integer));
		case Expression::Impl::Kind::Decimal: {
			auto value = ffi::visit_expression_literal_decimal(state, impl.high, impl.low, impl.precision, impl.scale,
			                                                   AllocateVisitorError);
			if (!Unpack(value, result, error)) {
				return ~uintptr_t(0);
			}
			return result;
		}
		}
		error = "unsupported SDK expression";
		return ~uintptr_t(0);
	}

	struct Iterator {
		PredicateBridge *bridge;
		ffi::KernelExpressionVisitorState *state;
		const std::vector<Predicate> *children;
		size_t offset = 0;
		static const void *Next(void *data) noexcept {
			auto &iterator = *static_cast<Iterator *>(data);
			if (iterator.offset == iterator.children->size()) {
				return nullptr;
			}
			auto id = iterator.bridge->VisitPredicate((*iterator.children)[iterator.offset++], iterator.state);
			return reinterpret_cast<const void *>(id);
		}
	};

	uintptr_t VisitPredicate(const Predicate &predicate, ffi::KernelExpressionVisitorState *state) noexcept {
		const auto &impl = *predicate.impl_;
		switch (impl.kind) {
		case Predicate::Impl::Kind::Compare: {
			auto left = VisitExpression(*impl.left, state);
			auto right = VisitExpression(*impl.right, state);
			switch (impl.comparison) {
			case Comparison::LessThan:
				return ffi::visit_predicate_lt(state, left, right);
			case Comparison::LessThanOrEqual:
				return ffi::visit_predicate_le(state, left, right);
			case Comparison::GreaterThan:
				return ffi::visit_predicate_gt(state, left, right);
			case Comparison::GreaterThanOrEqual:
				return ffi::visit_predicate_ge(state, left, right);
			case Comparison::Equal:
				return ffi::visit_predicate_eq(state, left, right);
			case Comparison::NotEqual:
				return ffi::visit_predicate_ne(state, left, right);
			}
		}
		case Predicate::Impl::Kind::And:
		case Predicate::Impl::Kind::Or: {
			Iterator iterator {this, state, &impl.children};
			ffi::EngineIterator ffi_iterator {&iterator, &Iterator::Next};
			return impl.kind == Predicate::Impl::Kind::And ? ffi::visit_predicate_and(state, &ffi_iterator)
			                                                : ffi::visit_predicate_or(state, &ffi_iterator);
		}
		case Predicate::Impl::Kind::Not:
			return ffi::visit_predicate_not(state, VisitPredicate(impl.children.front(), state));
		case Predicate::Impl::Kind::IsNull:
			return ffi::visit_predicate_is_null(state, VisitExpression(*impl.left, state));
		}
		error = "unsupported SDK predicate";
		return ~uintptr_t(0);
	}

	const Predicate &root;
	ffi::EnginePredicate ffi {};
	std::string error;
};

//===----------------------------------------------------------------------===//
// Raw handle owners hidden behind PImpl
//===----------------------------------------------------------------------===//

struct FinishedReducer::Impl {
	explicit Impl(ffi::DeltaFinishedReducer *handle) : handle(handle) {
	}
	~Impl() {
		if (handle) {
			ffi::delta_finished_reducer_free(handle);
		}
	}
	ffi::DeltaFinishedReducer *release() noexcept {
		auto *result = handle;
		handle = nullptr;
		return result;
	}
	ffi::DeltaFinishedReducer *handle;
};

struct Reducer::Impl {
	explicit Impl(ffi::DeltaReducer *handle) : handle(handle) {
	}
	~Impl() {
		if (handle) {
			ffi::delta_reducer_free(handle);
		}
	}
	ffi::DeltaReducer *handle;
};

struct SnapshotStateMachine::Impl {
	explicit Impl(ffi::DeltaSm *handle) : handle(handle) {
	}
	~Impl() {
		if (handle) {
			ffi::delta_sm_free(handle);
		}
	}
	ffi::DeltaSm *handle;
};

struct ScanStateMachine::Impl {
	explicit Impl(ffi::DeltaSm *handle) : handle(handle) {
	}
	~Impl() {
		if (handle) {
			ffi::delta_sm_free(handle);
		}
	}
	ffi::DeltaSm *handle;
};

struct Snapshot::Impl {
	explicit Impl(ffi::DeltaSnapshotValue *handle) : handle(handle), version(ffi::delta_snapshot_version(handle)) {
	}
	~Impl() {
		if (handle) {
			ffi::delta_snapshot_free(handle);
		}
	}
	ffi::DeltaSnapshotValue *handle;
	int64_t version;
	mutable std::optional<delta::schema::StructType> schema;
};

struct Scan::Impl {
	explicit Impl(plan::ResultPlan plan) : plan(std::move(plan)) {
	}
	plan::ResultPlan plan;
};

FinishedReducer::~FinishedReducer() = default;
FinishedReducer::FinishedReducer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
FinishedReducer::FinishedReducer(FinishedReducer &&) noexcept = default;
FinishedReducer &FinishedReducer::operator=(FinishedReducer &&) noexcept = default;
Reducer::~Reducer() = default;
Reducer::Reducer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
Reducer::Reducer(Reducer &&) noexcept = default;
Reducer &Reducer::operator=(Reducer &&) noexcept = default;
SnapshotStateMachine::~SnapshotStateMachine() = default;
SnapshotStateMachine::SnapshotStateMachine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
SnapshotStateMachine::SnapshotStateMachine(SnapshotStateMachine &&) noexcept = default;
SnapshotStateMachine &SnapshotStateMachine::operator=(SnapshotStateMachine &&) noexcept = default;
ScanStateMachine::~ScanStateMachine() = default;
ScanStateMachine::ScanStateMachine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
ScanStateMachine::ScanStateMachine(ScanStateMachine &&) noexcept = default;
ScanStateMachine &ScanStateMachine::operator=(ScanStateMachine &&) noexcept = default;
Snapshot::~Snapshot() = default;
Snapshot::Snapshot(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
Snapshot::Snapshot(Snapshot &&) noexcept = default;
Snapshot &Snapshot::operator=(Snapshot &&) noexcept = default;
Scan::~Scan() = default;
Scan::Scan(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
Scan::Scan(Scan &&) noexcept = default;
Scan &Scan::operator=(Scan &&) noexcept = default;

Control Reducer::apply(ArrowBatch &batch) {
	char *error = nullptr;
	static_assert(sizeof(ArrowArray) == sizeof(ffi::FFI_ArrowArray), "ArrowArray ABI mismatch");
	static_assert(sizeof(ArrowSchema) == sizeof(ffi::FFI_ArrowSchema), "ArrowSchema ABI mismatch");
	auto result = ffi::delta_reducer_apply(impl_->handle,
	                                       reinterpret_cast<ffi::FFI_ArrowArray *>(batch.mutable_array()),
	                                       reinterpret_cast<ffi::FFI_ArrowSchema *>(batch.mutable_schema()), &error);
	if (result < 0) {
		Throw("delta_reducer_apply", error);
	}
	return result == 1 ? Control::Break : Control::Continue;
}

FinishedReducer Reducer::finish() && {
	char *error = nullptr;
	auto *finished = ffi::delta_reducer_finish(impl_->handle, &error);
	impl_->handle = nullptr;
	if (!finished) {
		Throw("delta_reducer_finish", error);
	}
	return FinishedReducer(std::make_unique<FinishedReducer::Impl>(finished));
}

FinishedReducer Reducer::apply_all(ArrowStream stream) && {
	while (auto batch = stream()) {
		if (apply(*batch) == Control::Break) {
			break;
		}
	}
	return std::move(*this).finish();
}

struct SdkBridge {
	template <class Impl>
	static EngineRequest Next(Impl &impl) {
	char *error = nullptr;
	const auto step = ffi::delta_sm_next(impl.handle, &error);
	if (step < 0) {
		Throw("delta_sm_next", error);
	}
	if (step == 0) {
		return Done {};
	}
	size_t length = 0;
	auto *bytes = ffi::delta_sm_reduce_plan(impl.handle, &length, &error);
	auto plan = Decode<delta::plan::ResultPlan>("delta_sm_reduce_plan", bytes, length, error);
	auto *raw_reducer = ffi::delta_sm_take_reducer(impl.handle, &error);
	if (!raw_reducer) {
		Throw("delta_sm_take_reducer", error);
	}
	return Reduce {std::move(plan), Reducer(std::make_unique<Reducer::Impl>(raw_reducer))};
	}

	template <class Impl>
	static void Submit(Impl &impl, FinishedReducer result) {
	char *error = nullptr;
	if (ffi::delta_sm_submit(impl.handle, result.impl_->release(), &error) != 0) {
		Throw("delta_sm_submit", error);
	}
	}

	template <class Impl>
	[[noreturn]] static void SubmitError(Impl &impl, const EngineError &engine_error) {
	char *error = nullptr;
	ffi::delta_sm_submit_error(impl.handle, engine_error.message.data(), engine_error.message.size(), &error);
	Throw("engine reduce", error);
 	}
};

namespace {

template <class StateMachine, class Value>
Value Drive(StateMachine state_machine, Engine &engine) {
	for (;;) {
		auto request = state_machine.next();
		if (std::holds_alternative<Done>(request)) {
			return std::move(state_machine).build();
		}
		auto &reduce = std::get<Reduce>(request);
		try {
			auto stream = engine.execute_to_arrow(reduce.plan);
			auto finished = std::move(reduce.reducer).apply_all(std::move(stream));
			state_machine.submit(std::move(finished));
		} catch (const DeltaError &) {
			throw;
		} catch (const std::exception &error) {
			state_machine.submit(EngineError {error.what()});
		} catch (...) {
			state_machine.submit(EngineError {"unknown engine failure while executing reduce plan"});
		}
	}
}

} // namespace

EngineRequest SnapshotStateMachine::next() {
	return SdkBridge::Next(*impl_);
}
void SnapshotStateMachine::submit(FinishedReducer result) {
	SdkBridge::Submit(*impl_, std::move(result));
}
[[noreturn]] void SnapshotStateMachine::submit(const EngineError &error) {
	SdkBridge::SubmitError(*impl_, error);
}
Snapshot SnapshotStateMachine::build() && {
	char *error = nullptr;
	auto *snapshot = ffi::delta_sm_build_snapshot(impl_->handle, &error);
	impl_->handle = nullptr;
	if (!snapshot) {
		Throw("delta_sm_build_snapshot", error);
	}
	return Snapshot(std::make_unique<Snapshot::Impl>(snapshot));
}

EngineRequest ScanStateMachine::next() {
	return SdkBridge::Next(*impl_);
}
void ScanStateMachine::submit(FinishedReducer result) {
	SdkBridge::Submit(*impl_, std::move(result));
}
[[noreturn]] void ScanStateMachine::submit(const EngineError &error) {
	SdkBridge::SubmitError(*impl_, error);
}
Scan ScanStateMachine::build() && {
	char *error = nullptr;
	auto *scan = ffi::delta_sm_build_scan(impl_->handle, &error);
	impl_->handle = nullptr;
	if (!scan) {
		Throw("delta_sm_build_scan", error);
	}
	size_t length = 0;
	auto *bytes = ffi::delta_scan_plan(scan, &length, &error);
	auto result = Decode<plan::ResultPlan>("delta_scan_plan", bytes, length, error);
	ffi::delta_scan_free(scan);
	return Scan(std::make_unique<Scan::Impl>(std::move(result)));
}

int64_t Snapshot::version() const noexcept {
	return impl_->version;
}
const schema::StructType &Snapshot::schema() const {
	if (!impl_->schema) {
		char *error = nullptr;
		size_t length = 0;
		auto *bytes = ffi::delta_snapshot_schema(impl_->handle, &length, &error);
		impl_->schema = Decode<delta::schema::StructType>("delta_snapshot_schema", bytes, length, error);
	}
	return *impl_->schema;
}
ScanStateMachine Snapshot::scan_sm(ScanKind kind, const Predicate *predicate) const {
	char *error = nullptr;
	std::optional<PredicateBridge> bridge;
	if (predicate) {
		bridge.emplace(*predicate);
	}
	auto *state_machine = ffi::delta_snapshot_scan_sm(
	    impl_->handle, kind == ScanKind::Metadata ? 0 : 1, bridge ? &bridge->ffi : nullptr, &error);
	if (bridge && !bridge->error.empty()) {
		if (state_machine) {
			ffi::delta_sm_free(state_machine);
		}
		if (error) {
			ffi::delta_string_free(error);
		}
		throw DeltaError("predicate translation failed: " + bridge->error);
	}
	if (!state_machine) {
		Throw("delta_snapshot_scan_sm", error);
	}
	return ScanStateMachine(std::make_unique<ScanStateMachine::Impl>(state_machine));
}
Scan Snapshot::scan(Engine &engine, ScanKind kind, const Predicate *predicate) const {
	return drive(scan_sm(kind, predicate), engine);
}

const plan::ResultPlan &Scan::plan() const noexcept {
	return impl_->plan;
}

SnapshotStateMachine open_snapshot(const std::string &path, int64_t version) {
	char *error = nullptr;
	auto *state_machine = ffi::delta_open_snapshot(path.data(), path.size(), version, &error);
	if (!state_machine) {
		Throw("delta_open_snapshot", error);
	}
	return SnapshotStateMachine(std::make_unique<SnapshotStateMachine::Impl>(state_machine));
}

Snapshot drive(SnapshotStateMachine state_machine, Engine &engine) {
	return Drive<SnapshotStateMachine, Snapshot>(std::move(state_machine), engine);
}
Scan drive(ScanStateMachine state_machine, Engine &engine) {
	return Drive<ScanStateMachine, Scan>(std::move(state_machine), engine);
}

} // namespace delta
