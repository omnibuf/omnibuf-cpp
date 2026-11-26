#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "editions_2023.omnibuf.pb.hpp"
#include "editions_2023_implicit.omnibuf.pb.hpp"
#include "editions_2024.omnibuf.pb.hpp"
#include "editions_2026.omnibuf.pb.hpp"
#include "omnibuf/omnibuf.hpp"
#include "test_harness.hpp"

namespace {

// Compile-time verification of Editions field presence mapping.
static_assert(std::is_same_v<decltype(omnibuf::editions::e2023::Envelope::opt_i32),
                             std::optional<int32_t>>);
static_assert(
    std::is_same_v<decltype(omnibuf::editions::e2023::Envelope::opt_bool), std::optional<bool>>);
static_assert(std::is_same_v<decltype(omnibuf::editions::e2023::Envelope::opt_str),
                             std::optional<std::string>>);
static_assert(std::is_same_v<decltype(omnibuf::editions::e2023::Envelope::opt_bytes),
                             std::optional<std::vector<uint8_t>>>);
static_assert(std::is_same_v<decltype(omnibuf::editions::e2023::Envelope::opt_status),
                             std::optional<omnibuf::editions::e2023::E23Status>>);
static_assert(std::is_same_v<decltype(omnibuf::editions::e2023::Envelope::imp_i32), int32_t>);
static_assert(std::is_same_v<decltype(omnibuf::editions::e2023::Envelope::imp_bool), bool>);
static_assert(std::is_same_v<decltype(omnibuf::editions::e2023::Envelope::imp_str), std::string>);

static_assert(
    std::is_same_v<decltype(omnibuf::editions::e2023_imp::ImplicitEnvelope::implicit_i32),
                   int32_t>);
static_assert(
    std::is_same_v<decltype(omnibuf::editions::e2023_imp::ImplicitEnvelope::implicit_str),
                   std::string>);
static_assert(
    std::is_same_v<decltype(omnibuf::editions::e2023_imp::ImplicitEnvelope::explicit_i32),
                   std::optional<int32_t>>);
static_assert(
    std::is_same_v<decltype(omnibuf::editions::e2023_imp::ImplicitEnvelope::explicit_bool),
                   std::optional<bool>>);
static_assert(
    std::is_same_v<decltype(omnibuf::editions::e2023_imp::ImplicitEnvelope::explicit_str),
                   std::optional<std::string>>);

static_assert(std::is_same_v<decltype(omnibuf::editions::e2024::Edition2024Record::id),
                             std::optional<int32_t>>);
static_assert(
    std::is_same_v<decltype(omnibuf::editions::e2024::Edition2024Record::implicit_metric),
                   int32_t>);

static_assert(std::is_same_v<decltype(omnibuf::editions::e2026::Edition2026Record::count),
                             std::optional<int32_t>>);
static_assert(std::is_same_v<decltype(omnibuf::editions::e2026::Edition2026Record::implicit_rank),
                             int32_t>);

void test_e2023_explicit_presence_absent_vs_zero_values() {
  using omnibuf::editions::e2023::E23Status;
  using omnibuf::editions::e2023::Envelope;

  Envelope absent;
  const auto absent_wire = absent.marshal_omnibuf();

  Envelope zero_present;
  zero_present.opt_i32 = 0;
  zero_present.opt_u64 = 0ULL;
  zero_present.opt_f64 = 0.0;
  zero_present.opt_bool = false;
  zero_present.opt_str = std::string{};
  zero_present.opt_bytes = std::vector<uint8_t>{};
  zero_present.opt_status = E23Status::E23_STATUS_UNSPECIFIED;
  zero_present.zero_i32 = 0;
  zero_present.zero_f64 = 0.0;
  zero_present.zero_bool = false;
  zero_present.zero_str = std::string{};
  zero_present.zero_bytes = std::vector<uint8_t>{};
  zero_present.zero_status = E23Status::E23_STATUS_UNSPECIFIED;
  const auto zero_wire = zero_present.marshal_omnibuf();

  EXPECT(zero_wire != absent_wire);
  EXPECT(zero_wire.size() > absent_wire.size());

  // Verify WireBoolFalse tag is absent on absent_wire and present on zero_wire for opt_bool (#4).
  const auto absent_bool_loc = omnibuf::locate_field(absent_wire, 4u);
  EXPECT(absent_bool_loc.packet_valid && !absent_bool_loc.ok);
  const auto zero_bool_loc = omnibuf::locate_field(zero_wire, 4u);
  EXPECT(zero_bool_loc.packet_valid && zero_bool_loc.ok &&
         zero_bool_loc.wire_type == omnibuf::WireBoolFalse);

  // Round-trip absent packet.
  Envelope dec_absent;
  dec_absent.opt_i32 = 99;
  dec_absent.opt_bool = true;
  dec_absent.opt_str = "stale";
  dec_absent.opt_bytes = {1, 2};
  dec_absent.opt_status = E23Status::E23_STATUS_OK;
  EXPECT(dec_absent.unmarshal_omnibuf(absent_wire).ok());
  EXPECT(!dec_absent.opt_i32.has_value());
  EXPECT(!dec_absent.opt_u64.has_value());
  EXPECT(!dec_absent.opt_f64.has_value());
  EXPECT(!dec_absent.opt_bool.has_value());
  EXPECT(!dec_absent.opt_str.has_value());
  EXPECT(!dec_absent.opt_bytes.has_value());
  EXPECT(!dec_absent.opt_status.has_value());
  EXPECT(!dec_absent.zero_i32.has_value());
  EXPECT(!dec_absent.zero_bool.has_value());
  EXPECT(!dec_absent.zero_str.has_value());
  EXPECT(!dec_absent.zero_status.has_value());
  EXPECT(dec_absent == absent);

  // Round-trip explicit-zero packet.
  Envelope dec_zero;
  EXPECT(dec_zero.unmarshal_omnibuf(zero_wire).ok());
  EXPECT(dec_zero.opt_i32.has_value() && *dec_zero.opt_i32 == 0);
  EXPECT(dec_zero.opt_u64.has_value() && *dec_zero.opt_u64 == 0ULL);
  EXPECT(dec_zero.opt_f64.has_value() && *dec_zero.opt_f64 == 0.0);
  EXPECT(dec_zero.opt_bool.has_value() && *dec_zero.opt_bool == false);
  EXPECT(dec_zero.opt_str.has_value() && dec_zero.opt_str->empty());
  EXPECT(dec_zero.opt_bytes.has_value() && dec_zero.opt_bytes->empty());
  EXPECT(dec_zero.opt_status.has_value() &&
         *dec_zero.opt_status == E23Status::E23_STATUS_UNSPECIFIED);
  EXPECT(dec_zero.zero_i32.has_value() && *dec_zero.zero_i32 == 0);
  EXPECT(dec_zero.zero_f64.has_value() && *dec_zero.zero_f64 == 0.0);
  EXPECT(dec_zero.zero_bool.has_value() && *dec_zero.zero_bool == false);
  EXPECT(dec_zero.zero_str.has_value() && dec_zero.zero_str->empty());
  EXPECT(dec_zero.zero_bytes.has_value() && dec_zero.zero_bytes->empty());
  EXPECT(dec_zero.zero_status.has_value() &&
         *dec_zero.zero_status == E23Status::E23_STATUS_UNSPECIFIED);
  EXPECT(dec_zero == zero_present);
}

void test_e2023_open_enums_collections_delimited_and_oneof() {
  using omnibuf::editions::e2023::Child;
  using omnibuf::editions::e2023::E23Status;
  using omnibuf::editions::e2023::Envelope;

  Envelope msg;
  // Open enum: unknown positive and negative values preserved.
  msg.opt_status = static_cast<E23Status>(777);
  msg.zero_status = static_cast<E23Status>(-999);
  msg.nested_kind = static_cast<Envelope::NestedKind>(42);
  msg.imp_i32 = -15;
  msg.imp_bool = true;
  msg.imp_str = "implicit";

  msg.packed_nums = {1, -2, 300};
  msg.expanded_nums = {0, -50, 99999};
  msg.tags = {"alpha", "", "omega"};
  msg.children_list = {Child{.id = 1, .note = "first", .active = true},
                       Child{.id = 0, .note = "", .active = false}};
  msg.scores = {{"zero", 0}, {"neg", -42}, {"pos", 100}};
  msg.children_map["k1"] = Child{.id = 10, .note = "mapped", .active = false};

  msg.regular_child = Child{.id = 20, .note = "regular", .active = true};
  msg.delimited_child = Child{.id = 21, .note = "delim", .active = false};
  msg.delimited_list = {Child{.id = 30, .note = "d0", .active = true},
                        Child{.id = 31, .note = "", .active = false}};
  msg.detail = Envelope::Detail{.info = "legacy-json-ok"};

  // Oneof active explicit zero / false / empty / delimited transitions.
  msg.set_choice_num(0);
  EXPECT(msg.choice_case() == Envelope::ChoiceCase::kChoiceNum);
  EXPECT(msg.has_choice_num() && msg.choice_num == 0);
  {
    const auto w = msg.marshal_omnibuf();
    Envelope dec;
    EXPECT(dec.unmarshal_omnibuf(w).ok());
    EXPECT(dec.choice_case() == Envelope::ChoiceCase::kChoiceNum);
    EXPECT(dec.has_choice_num() && dec.choice_num == 0);
    EXPECT(dec == msg);
  }

  msg.set_choice_flag(false);
  EXPECT(msg.choice_case() == Envelope::ChoiceCase::kChoiceFlag);
  EXPECT(!msg.has_choice_num() && msg.has_choice_flag() && !msg.choice_flag);
  {
    const auto w = msg.marshal_omnibuf();
    Envelope dec;
    EXPECT(dec.unmarshal_omnibuf(w).ok());
    EXPECT(dec.choice_case() == Envelope::ChoiceCase::kChoiceFlag);
    EXPECT(dec.has_choice_flag() && dec.choice_flag == false);
    EXPECT(dec == msg);
  }

  msg.set_choice_text("");
  EXPECT(msg.choice_case() == Envelope::ChoiceCase::kChoiceText);
  {
    const auto w = msg.marshal_omnibuf();
    Envelope dec;
    EXPECT(dec.unmarshal_omnibuf(w).ok());
    EXPECT(dec.choice_case() == Envelope::ChoiceCase::kChoiceText);
    EXPECT(dec.has_choice_text() && dec.choice_text.empty());
    EXPECT(dec == msg);
  }

  msg.set_choice_delimited(Child{.id = 99, .note = "oneof-delim", .active = true});
  {
    const auto w = msg.marshal_omnibuf();
    Envelope dec;
    EXPECT(dec.unmarshal_omnibuf(w).ok());
    EXPECT(dec.choice_case() == Envelope::ChoiceCase::kChoiceDelimited);
    EXPECT(dec.has_choice_delimited() && dec.choice_delimited.has_value());
    EXPECT(dec.choice_delimited->id == 99);
    EXPECT(dec.choice_delimited->note == "oneof-delim");
    EXPECT(dec == msg);
  }
}

void test_e2023_implicit_file_and_field_overrides() {
  using omnibuf::editions::e2023_imp::ImplicitEnvelope;
  using omnibuf::editions::e2023_imp::OpaqueImplicitEnvelope;

  ImplicitEnvelope absent;
  const auto absent_wire = absent.marshal_omnibuf();

  ImplicitEnvelope present_zero;
  present_zero.explicit_i32 = 0;
  present_zero.explicit_bool = false;
  present_zero.explicit_str = "";
  const auto zero_wire = present_zero.marshal_omnibuf();

  EXPECT(zero_wire != absent_wire);

  ImplicitEnvelope dec;
  EXPECT(dec.unmarshal_omnibuf(absent_wire).ok());
  EXPECT(!dec.explicit_i32.has_value());
  EXPECT(!dec.explicit_bool.has_value());
  EXPECT(!dec.explicit_str.has_value());
  EXPECT(dec.implicit_i32 == 0 && dec.implicit_str.empty());

  EXPECT(dec.unmarshal_omnibuf(zero_wire).ok());
  EXPECT(dec.explicit_i32.has_value() && *dec.explicit_i32 == 0);
  EXPECT(dec.explicit_bool.has_value() && *dec.explicit_bool == false);
  EXPECT(dec.explicit_str.has_value() && dec.explicit_str->empty());

  OpaqueImplicitEnvelope op(zero_wire);
  EXPECT(op.get_explicit_i32() == std::optional<int32_t>{0});
  EXPECT(op.get_explicit_bool() == std::optional<bool>{false});
  EXPECT(op.get_explicit_str() == std::optional<std::string>{""});
}

void test_e2023_opaque_and_view_presence_mutation_and_splicing() {
  using omnibuf::editions::e2023::Child;
  using omnibuf::editions::e2023::E23Status;
  using omnibuf::editions::e2023::Envelope;
  using omnibuf::editions::e2023::EnvelopeView;
  using omnibuf::editions::e2023::OpaqueEnvelope;

  // 1. Read absent vs present-zero on OpaqueEnvelope and EnvelopeView.
  Envelope absent;
  absent.imp_i32 = 5;
  absent.delimited_child = Child{.id = 7, .note = "init", .active = false};
  absent.delimited_list = {Child{.id = 1, .note = "a", .active = true}};
  absent.scores = {{"s1", 10}};
  absent.children_map["c1"] = Child{.id = 2, .note = "m", .active = true};
  const auto absent_wire = absent.marshal_omnibuf();

  {
    OpaqueEnvelope op(absent_wire);
    EXPECT(!op.get_opt_i32().has_value());
    EXPECT(!op.get_opt_f64().has_value());
    EXPECT(!op.get_opt_bool().has_value());
    EXPECT(!op.get_opt_str().has_value());
    EXPECT(!op.get_opt_bytes().has_value());
    EXPECT(!op.get_opt_status().has_value());
    EXPECT(op.get_imp_i32() == 5);
    EXPECT(op.get_delimited_child().get_id() == std::optional<int32_t>{7});
    EXPECT(op.get_delimited_child().get_active() == std::optional<bool>{false});
    EXPECT(op.delimited_list_len() == 1u);
    EXPECT(op.get_delimited_list(0).get_note() == std::optional<std::string>{"a"});
    const auto [sc_val, sc_ok] = op.lookup_scores("s1");
    EXPECT(sc_ok && sc_val == 10);
    const auto [cm_view, cm_ok] = op.lookup_children_map("c1");
    EXPECT(cm_ok && cm_view.get_id() == std::optional<int32_t>{2});

    EnvelopeView view(absent_wire);
    EXPECT(!view.get_opt_i32().has_value());
    EXPECT(!view.get_opt_bool().has_value());
    EXPECT(!view.get_opt_str().has_value());
    EXPECT(!view.get_opt_bytes().has_value());
    EXPECT(!view.get_opt_status().has_value());
    EXPECT(view.get_delimited_child().get_id() == std::optional<int32_t>{7});

    // Mutate absent -> explicit zero/false/empty and splice.
    op.set_opt_i32(0);
    op.set_opt_f64(0.0);
    op.set_opt_bool(false);
    op.set_opt_str(std::string{});
    op.set_opt_bytes(std::vector<uint8_t>{});
    op.set_opt_status(E23Status::E23_STATUS_UNSPECIFIED);
    op.set_choice_flag(false);

    EXPECT(op.get_opt_i32() == std::optional<int32_t>{0});
    EXPECT(op.get_opt_f64() == std::optional<double>{0.0});
    EXPECT(op.get_opt_bool() == std::optional<bool>{false});
    EXPECT(op.get_opt_str() == std::optional<std::string>{""});
    EXPECT(op.get_opt_bytes().has_value() && op.get_opt_bytes()->empty());
    EXPECT(op.get_opt_status() == std::optional<E23Status>{E23Status::E23_STATUS_UNSPECIFIED});

    const auto spliced_zero = op.try_marshal();
    EXPECT_MSG(spliced_zero.ok(), spliced_zero.error().c_str());

    Envelope dec;
    EXPECT(dec.unmarshal_omnibuf(spliced_zero.value()).ok());
    EXPECT(dec.opt_i32 == std::optional<int32_t>{0});
    EXPECT(dec.opt_f64 == std::optional<double>{0.0});
    EXPECT(dec.opt_bool == std::optional<bool>{false});
    EXPECT(dec.opt_str == std::optional<std::string>{""});
    EXPECT(dec.opt_bytes.has_value() && dec.opt_bytes->empty());
    EXPECT(dec.opt_status == std::optional<E23Status>{E23Status::E23_STATUS_UNSPECIFIED});
    EXPECT(dec.choice_case() == Envelope::ChoiceCase::kChoiceFlag && dec.choice_flag == false);

    // Now wrap spliced_zero in a fresh OpaqueEnvelope, verify view reads present-zero,
    // then clear all explicit fields back to std::nullopt and re-splice!
    EnvelopeView zero_view(spliced_zero.value());
    EXPECT(zero_view.get_opt_i32() == std::optional<int32_t>{0});
    EXPECT(zero_view.get_opt_bool() == std::optional<bool>{false});
    EXPECT(zero_view.get_opt_str() == std::optional<std::string_view>{""});
    EXPECT(zero_view.get_opt_bytes().has_value() && zero_view.get_opt_bytes()->empty());
    EXPECT(zero_view.get_opt_status() ==
           std::optional<E23Status>{E23Status::E23_STATUS_UNSPECIFIED});

    OpaqueEnvelope op_clear(spliced_zero.value());
    EXPECT(op_clear.try_get_opt_i32().ok() &&
           op_clear.try_get_opt_i32().value() == std::optional<int32_t>{0});
    EXPECT(op_clear.try_get_opt_bool().ok() &&
           op_clear.try_get_opt_bool().value() == std::optional<bool>{false});
    EXPECT(op_clear.try_get_opt_str().ok() &&
           op_clear.try_get_opt_str().value() == std::optional<std::string>{""});

    op_clear.set_opt_i32(std::nullopt);
    op_clear.set_opt_f64(std::nullopt);
    op_clear.set_opt_bool(std::nullopt);
    op_clear.set_opt_str(std::nullopt);
    op_clear.set_opt_bytes(std::nullopt);
    op_clear.set_opt_status(std::nullopt);
    op_clear.set_delimited_child(std::nullopt);

    const auto cleared_wire = op_clear.try_marshal();
    EXPECT_MSG(cleared_wire.ok(), cleared_wire.error().c_str());
    EXPECT(cleared_wire.value().size() < spliced_zero.value().size());

    Envelope dec_cleared;
    EXPECT(dec_cleared.unmarshal_omnibuf(cleared_wire.value()).ok());
    EXPECT(!dec_cleared.opt_i32.has_value());
    EXPECT(!dec_cleared.opt_f64.has_value());
    EXPECT(!dec_cleared.opt_bool.has_value());
    EXPECT(!dec_cleared.opt_str.has_value());
    EXPECT(!dec_cleared.opt_bytes.has_value());
    EXPECT(!dec_cleared.opt_status.has_value());
    EXPECT(!dec_cleared.delimited_child.has_value());
    EXPECT(dec_cleared.imp_i32 == 5);
  }

  // 2. Dirty optional string invalid UTF-8 rejected on both try_get and try_marshal.
  {
    OpaqueEnvelope op(absent_wire);
    op.set_opt_str(std::string("\xC3\x28", 2));
    EXPECT(!op.try_get_opt_str().ok());
    EXPECT(!op.try_marshal().ok());
  }
}

void test_e2024_strict_visibility_and_features_roundtrip() {
  using omnibuf::editions::e2024::E24State;
  using omnibuf::editions::e2024::Edition2024Record;
  using omnibuf::editions::e2024::Edition2024RecordView;
  using omnibuf::editions::e2024::LocalNote;
  using omnibuf::editions::e2024::OpaqueEdition2024Record;

  Edition2024Record absent;
  const auto absent_wire = absent.marshal_omnibuf();

  Edition2024Record rec;
  rec.id = 0;
  rec.enabled = false;
  rec.label = "";
  rec.blob = std::vector<uint8_t>{};
  rec.state = static_cast<E24State>(55);
  rec.implicit_metric = 123;
  rec.note = LocalNote{.text = "delimited-2024", .code = 0};
  rec.notes = {LocalNote{.text = "", .code = 1}, LocalNote{.text = "two", .code = 2}};
  rec.expanded_vals = {3, 1, 4};
  rec.counters = {{"a", 1}, {"z", 0}};
  rec.by_key["n"] = LocalNote{.text = "in-map", .code = -7};
  rec.set_ok(false);

  const auto wire = rec.marshal_omnibuf();
  EXPECT(wire != absent_wire);

  Edition2024Record dec;
  EXPECT(dec.unmarshal_omnibuf(wire).ok());
  EXPECT(dec == rec);
  EXPECT(dec.id == std::optional<int32_t>{0});
  EXPECT(dec.enabled == std::optional<bool>{false});
  EXPECT(dec.label == std::optional<std::string>{""});
  EXPECT(dec.blob.has_value() && dec.blob->empty());
  EXPECT(dec.state == std::optional<E24State>{static_cast<E24State>(55)});
  EXPECT(dec.body_case() == Edition2024Record::BodyCase::kOk && !dec.ok);

  OpaqueEdition2024Record op(wire);
  EXPECT(op.get_id() == std::optional<int32_t>{0});
  EXPECT(op.get_enabled() == std::optional<bool>{false});
  EXPECT(op.get_note().get_text() == std::optional<std::string>{"delimited-2024"});
  EXPECT(op.notes_len() == 2u);
  op.set_msg("switched");
  op.set_id(std::nullopt);
  const auto spliced = op.try_marshal();
  EXPECT(spliced.ok());
  Edition2024Record dec2;
  EXPECT(dec2.unmarshal_omnibuf(spliced.value()).ok());
  EXPECT(!dec2.id.has_value());
  EXPECT(dec2.body_case() == Edition2024Record::BodyCase::kMsg && dec2.msg == "switched");

  Edition2024RecordView view(spliced.value());
  EXPECT(!view.get_id().has_value());
  EXPECT(view.get_msg() == "switched");
}

void test_e2026_full_roundtrip_opaque_and_view() {
  using omnibuf::editions::e2026::E26Child;
  using omnibuf::editions::e2026::E26Mode;
  using omnibuf::editions::e2026::Edition2026Record;
  using omnibuf::editions::e2026::Edition2026RecordView;
  using omnibuf::editions::e2026::OpaqueEdition2026Record;

  Edition2026Record absent;
  const auto absent_wire = absent.marshal_omnibuf();

  Edition2026Record zero_rec;
  zero_rec.count = 0;
  zero_rec.ratio = 0.0;
  zero_rec.active = false;
  zero_rec.title = "";
  zero_rec.raw = std::vector<uint8_t>{};
  zero_rec.mode = E26Mode::E26_MODE_UNSPECIFIED;
  const auto zero_wire = zero_rec.marshal_omnibuf();
  EXPECT(zero_wire != absent_wire);

  Edition2026Record dec_zero;
  EXPECT(dec_zero.unmarshal_omnibuf(zero_wire).ok());
  EXPECT(dec_zero == zero_rec);
  EXPECT(dec_zero.count == std::optional<int32_t>{0});
  EXPECT(dec_zero.ratio == std::optional<double>{0.0});
  EXPECT(dec_zero.active == std::optional<bool>{false});
  EXPECT(dec_zero.title == std::optional<std::string>{""});
  EXPECT(dec_zero.raw.has_value() && dec_zero.raw->empty());
  EXPECT(dec_zero.mode == std::optional<E26Mode>{E26Mode::E26_MODE_UNSPECIFIED});

  Edition2026Record full;
  full.count = 42;
  full.ratio = 3.5;
  full.active = true;
  full.title = "edition-2026";
  full.raw = {0xDE, 0xAD, 0xBE, 0xEF};
  full.mode = static_cast<E26Mode>(1234);
  full.implicit_rank = 9;
  full.delimited_child = E26Child{.seq = 1, .tag = "d1"};
  full.delimited_items = {E26Child{.seq = 2, .tag = "d2"}, E26Child{.seq = 0, .tag = ""}};
  full.expanded_ids = {10, 20, 30};
  full.weights = {{"w1", 100}, {"w0", 0}};
  full.children["c"] = E26Child{.seq = 5, .tag = "child"};
  full.set_text("");

  const auto full_wire = full.marshal_omnibuf();
  Edition2026Record dec_full;
  EXPECT(dec_full.unmarshal_omnibuf(full_wire).ok());
  EXPECT(dec_full == full);
  EXPECT(dec_full.variant_case() == Edition2026Record::VariantCase::kText &&
         dec_full.text.empty());

  OpaqueEdition2026Record op(full_wire);
  EXPECT(op.get_count() == std::optional<int32_t>{42});
  EXPECT(op.get_mode() == std::optional<E26Mode>{static_cast<E26Mode>(1234)});
  EXPECT(op.get_delimited_child().get_tag() == std::optional<std::string>{"d1"});
  EXPECT(op.delimited_items_len() == 2u);
  op.set_count(0);
  op.set_active(false);
  op.set_title(std::nullopt);
  op.set_num(0);
  const auto spliced = op.try_marshal();
  EXPECT_MSG(spliced.ok(), spliced.error().c_str());

  Edition2026Record dec_spliced;
  EXPECT(dec_spliced.unmarshal_omnibuf(spliced.value()).ok());
  EXPECT(dec_spliced.count == std::optional<int32_t>{0});
  EXPECT(dec_spliced.active == std::optional<bool>{false});
  EXPECT(!dec_spliced.title.has_value());
  EXPECT(dec_spliced.variant_case() == Edition2026Record::VariantCase::kNum &&
         dec_spliced.num == 0);

  Edition2026RecordView view(spliced.value());
  EXPECT(view.get_count() == std::optional<int32_t>{0});
  EXPECT(view.get_active() == std::optional<bool>{false});
  EXPECT(!view.get_title().has_value());
  EXPECT(view.get_num() == 0);
}

void test_runtime_descriptor_pool_and_dynamic_message_editions() {
#ifdef OMNIBUF_EDITIONS_FDS_PATH
  omnibuf::proto::DescriptorPool pool;
  const auto load_res = pool.load_file(OMNIBUF_EDITIONS_FDS_PATH);
  EXPECT_MSG(load_res.ok(), load_res.error().c_str());

  // 1. Edition 2023 absent vs explicit zero parity with DynamicMessage.
  const auto* d23 = pool.find_message("omnibuf.editions.e2023.Envelope");
  EXPECT(d23 != nullptr);
  if (d23 != nullptr) {
    omnibuf::editions::e2023::Envelope absent;
    const auto absent_wire = absent.marshal_omnibuf({.deterministic = true});
    omnibuf::proto::DynamicMessage dyn_abs;
    EXPECT(omnibuf::proto::from_binary(*d23, absent_wire, dyn_abs).ok());
    const auto dyn_abs_wire = omnibuf::proto::to_binary(*d23, dyn_abs, {.deterministic = true});
    EXPECT(dyn_abs_wire.ok());
    EXPECT(dyn_abs_wire.value() == absent_wire);

    omnibuf::editions::e2023::Envelope zero;
    zero.opt_i32 = 0;
    zero.opt_bool = false;
    zero.opt_str = "";
    zero.opt_bytes = std::vector<uint8_t>{};
    zero.opt_status = omnibuf::editions::e2023::E23Status::E23_STATUS_UNSPECIFIED;
    zero.delimited_child = omnibuf::editions::e2023::Child{.id = 0, .note = "", .active = false};
    zero.set_choice_flag(false);
    const auto zero_wire = zero.marshal_omnibuf({.deterministic = true});

    omnibuf::proto::DynamicMessage dyn_zero;
    EXPECT(omnibuf::proto::from_binary(*d23, zero_wire, dyn_zero).ok());
    const auto dyn_zero_wire = omnibuf::proto::to_binary(*d23, dyn_zero, {.deterministic = true});
    EXPECT(dyn_zero_wire.ok());
    EXPECT(dyn_zero_wire.value() == zero_wire);
  }

  // 2. Edition 2024 & 2026 parity with DynamicMessage.
  const auto* d24 = pool.find_message("omnibuf.editions.e2024.Edition2024Record");
  EXPECT(d24 != nullptr);
  if (d24 != nullptr) {
    omnibuf::editions::e2024::Edition2024Record r24;
    r24.id = 0;
    r24.enabled = false;
    r24.label = "";
    r24.note = omnibuf::editions::e2024::LocalNote{.text = "hi", .code = 0};
    const auto w24 = r24.marshal_omnibuf({.deterministic = true});
    omnibuf::proto::DynamicMessage dyn24;
    EXPECT(omnibuf::proto::from_binary(*d24, w24, dyn24).ok());
    const auto bw24 = omnibuf::proto::to_binary(*d24, dyn24, {.deterministic = true});
    EXPECT(bw24.ok() && bw24.value() == w24);
  }

  const auto* d26 = pool.find_message("omnibuf.editions.e2026.Edition2026Record");
  EXPECT(d26 != nullptr);
  if (d26 != nullptr) {
    omnibuf::editions::e2026::Edition2026Record r26;
    r26.count = 0;
    r26.active = false;
    r26.title = "";
    r26.delimited_child = omnibuf::editions::e2026::E26Child{.seq = 7, .tag = "t"};
    const auto w26 = r26.marshal_omnibuf({.deterministic = true});
    omnibuf::proto::DynamicMessage dyn26;
    EXPECT(omnibuf::proto::from_binary(*d26, w26, dyn26).ok());
    const auto bw26 = omnibuf::proto::to_binary(*d26, dyn26, {.deterministic = true});
    EXPECT(bw26.ok() && bw26.value() == w26);
  }
#endif
}

#if !defined(_WIN32) && defined(OMNIBUF_PROTOC_BIN) && defined(OMNIBUF_PLUGIN_BIN)
struct ProtocRunResult {
  int exit_code = -1;
  std::string stderr_output{};
};

ProtocRunResult run_protoc_with_plugin(const std::filesystem::path& dir,
                                       const std::string& target_proto) {
  ProtocRunResult out;
  const std::filesystem::path out_dir = dir / "out";
  std::filesystem::create_directories(out_dir);

  int err_pipe[2] = {-1, -1};
  if (::pipe(err_pipe) != 0) return out;

  const std::string plugin_arg = std::string("--plugin=protoc-gen-omnibuf-cpp=") +
                                 OMNIBUF_PLUGIN_BIN;
  const std::string out_arg = "--omnibuf-cpp_out=" + out_dir.string();
  const std::string inc_arg = "-I" + dir.string();
  const std::string proto_arg = (dir / target_proto).string();

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return out;
  }
  if (pid == 0) {
    ::close(err_pipe[0]);
    (void)::dup2(err_pipe[1], STDERR_FILENO);
    (void)::dup2(err_pipe[1], STDOUT_FILENO);
    ::close(err_pipe[1]);
    char* const argv[] = {
        const_cast<char*>(OMNIBUF_PROTOC_BIN),
        const_cast<char*>(plugin_arg.c_str()),
        const_cast<char*>(out_arg.c_str()),
        const_cast<char*>(inc_arg.c_str()),
        const_cast<char*>(proto_arg.c_str()),
        nullptr,
    };
    ::execv(OMNIBUF_PROTOC_BIN, argv);
    ::_exit(127);
  }

  ::close(err_pipe[1]);
  char buf[4096];
  while (true) {
    const ssize_t n = ::read(err_pipe[0], buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    out.stderr_output.append(buf, static_cast<size_t>(n));
  }
  ::close(err_pipe[0]);

  int status = 0;
  (void)::waitpid(pid, &status, 0);
  out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return out;
}

void write_text_file(const std::filesystem::path& p, std::string_view text) {
  std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
  ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void test_real_protoc_rejects_invalid_editions_schemas() {
  struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };

  std::string tmpl = (std::filesystem::temp_directory_path() / "omnibuf_ed_XXXXXX").string();
  std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
  tmpl_buf.push_back('\0');
  char* made = ::mkdtemp(tmpl_buf.data());
  EXPECT(made != nullptr);
  if (made == nullptr) return;
  TempDirGuard guard{std::filesystem::path(made)};

  struct NegCase {
    std::string name;
    std::string content;
    std::string expected_substr;
    std::string extra_file_name{};
    std::string extra_file_content{};
  };

  const std::vector<NegCase> neg_cases = {
      {
          "closed_2023.proto",
          "edition = \"2023\";\npackage neg;\nenum Bad {\n  option features.enum_type = CLOSED;\n"
          "  BAD_ZERO = 0;\n}\nmessage M { Bad b = 1; }\n",
          "CLOSED",
      },
      {
          "closed_2026.proto",
          "edition = \"2026\";\npackage neg26;\nexport enum Bad {\n"
          "  option features.enum_type = CLOSED;\n  BAD_ZERO = 0;\n}\n"
          "export message M { Bad b = 1; }\n",
          "CLOSED",
      },
      {
          "import_p2_closed.proto",
          "edition = \"2023\";\npackage neg;\nimport \"legacy_enum.proto\";\n"
          "message M { legacy.LegacyClosed e = 1; }\n",
          "CLOSED",
          "legacy_enum.proto",
          "syntax = \"proto2\";\npackage legacy;\nenum LegacyClosed { LEGACY_ZERO = 0; }\n",
      },
      {
          "legacy_required.proto",
          "edition = \"2023\";\npackage neg;\nmessage M {\n"
          "  int32 req = 1 [features.field_presence = LEGACY_REQUIRED];\n}\n",
          "LEGACY_REQUIRED",
      },
      {
          "utf8_none_map.proto",
          "edition = \"2023\";\npackage neg;\nmessage M {\n"
          "  map<string, int32> kv = 1 [features.utf8_validation = NONE];\n}\n",
          "UTF-8 NONE",
      },
      {
          "nonzero_default_scalar.proto",
          "edition = \"2023\";\npackage neg;\nmessage M { int32 x = 1 [default = 7]; }\n",
          "custom default values",
      },
      {
          "nonzero_default_enum.proto",
          "edition = \"2023\";\npackage neg;\nenum E { E_ZERO = 0; E_ONE = 1; }\n"
          "message M { E e = 1 [default = E_ONE]; }\n",
          "custom default values",
      },
      {
          "non_option_ext.proto",
          "edition = \"2023\";\npackage neg;\nmessage Base { int32 a = 1; extensions 100 to 199; }\n",
          "extensions",
      },
  };

  for (const auto& nc : neg_cases) {
    const std::filesystem::path case_dir = guard.path / nc.name.substr(0, nc.name.size() - 6);
    std::filesystem::create_directories(case_dir);
    if (!nc.extra_file_name.empty()) {
      write_text_file(case_dir / nc.extra_file_name, nc.extra_file_content);
    }
    write_text_file(case_dir / nc.name, nc.content);

    const auto res = run_protoc_with_plugin(case_dir, nc.name);
    EXPECT_MSG(res.exit_code != 0, nc.name.c_str());
    EXPECT_MSG(res.stderr_output.find(nc.expected_substr) != std::string::npos,
               res.stderr_output.c_str());
    EXPECT_MSG(res.stderr_output.find("does not support editions") == std::string::npos,
               res.stderr_output.c_str());
  }
}
#endif

}  // namespace

int main() {
  RUN_CASE(test_e2023_explicit_presence_absent_vs_zero_values);
  RUN_CASE(test_e2023_open_enums_collections_delimited_and_oneof);
  RUN_CASE(test_e2023_implicit_file_and_field_overrides);
  RUN_CASE(test_e2023_opaque_and_view_presence_mutation_and_splicing);
  RUN_CASE(test_e2024_strict_visibility_and_features_roundtrip);
  RUN_CASE(test_e2026_full_roundtrip_opaque_and_view);
  RUN_CASE(test_runtime_descriptor_pool_and_dynamic_message_editions);
#if !defined(_WIN32) && defined(OMNIBUF_PROTOC_BIN) && defined(OMNIBUF_PLUGIN_BIN)
  RUN_CASE(test_real_protoc_rejects_invalid_editions_schemas);
#endif

  std::printf("\nEditions E2E Test Summary: %d passed, %d failed\n", omnibuf_test::g_passed,
              omnibuf_test::g_failed);
  return omnibuf_test::g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
