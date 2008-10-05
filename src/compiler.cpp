/* Copyright (c) 2008, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "compiler.h"
#include "assembler.h"

using namespace vm;

namespace {

const bool DebugAppend = false;
const bool DebugCompile = false;
const bool DebugStack = false;
const bool DebugRegisters = false;
const bool DebugFrameIndexes = false;

const int AnyFrameIndex = -2;
const int NoFrameIndex = -1;

class Context;
class Value;
class Stack;
class Site;
class RegisterSite;
class MemorySite;
class Event;
class PushEvent;
class Read;
class MultiRead;
class StubRead;
class Block;

void NO_RETURN abort(Context*);

void
apply(Context* c, UnaryOperation op,
      unsigned s1Size, Site* s1);

void
apply(Context* c, BinaryOperation op,
      unsigned s1Size, Site* s1,
      unsigned s2Size, Site* s2);

void
apply(Context* c, TernaryOperation op,
      unsigned s1Size, Site* s1,
      unsigned s2Size, Site* s2,
      unsigned s3Size, Site* s3);

enum ConstantCompare {
  CompareNone,
  CompareLess,
  CompareGreater,
  CompareEqual
};

class Cell {
 public:
  Cell(Cell* next, void* value): next(next), value(value) { }

  Cell* next;
  void* value;
};

class Local {
 public:
  Value* value;
  unsigned size;
};

class Site {
 public:
  Site(): next(0) { }
  
  virtual ~Site() { }

  virtual Site* readTarget(Context*, Read*) { return this; }

  virtual void toString(Context*, char*, unsigned) = 0;

  virtual unsigned copyCost(Context*, Site*) = 0;

  virtual bool match(Context*, uint8_t, uint64_t, int) = 0;
  
  virtual void acquire(Context*, Stack*, Local*, unsigned, Value*) { }

  virtual void release(Context*) { }

  virtual void freeze(Context*) { }

  virtual void thaw(Context*) { }

  virtual OperandType type(Context*) = 0;

  virtual Assembler::Operand* asAssemblerOperand(Context*) = 0;

  virtual void makeSpecific(Context*) { }

  Site* next;
};

class Stack: public Compiler::StackElement {
 public:
  Stack(unsigned index, unsigned size, Value* value, Stack* next):
    index(index), size(size), padding(0), value(value), next(next)
  { }

  unsigned index;
  unsigned size;
  unsigned padding;
  Value* value;
  Stack* next;
};

class MultiReadPair {
 public:
  Value* value;
  MultiRead* read;
};

class MyState: public Compiler::State {
 public:
  MyState(Stack* stack, Local* locals, Event* predecessor,
          unsigned logicalIp):
    stack(stack),
    locals(locals),
    predecessor(predecessor),
    logicalIp(logicalIp),
    readCount(0)
  { }

  Stack* stack;
  Local* locals;
  Event* predecessor;
  unsigned logicalIp;
  unsigned readCount;
  MultiReadPair reads[0];
};

class LogicalInstruction {
 public:
  LogicalInstruction(int index, Stack* stack, Local* locals):
    firstEvent(0), lastEvent(0), immediatePredecessor(0), stack(stack),
    locals(locals), machineOffset(0), index(index)
  { }

  Event* firstEvent;
  Event* lastEvent;
  LogicalInstruction* immediatePredecessor;
  Stack* stack;
  Local* locals;
  Promise* machineOffset;
  int index;
};

class Register {
 public:
  Register(int number):
    value(0), site(0), number(number), size(0), refCount(0),
    freezeCount(0), reserved(false)
  { }

  Value* value;
  RegisterSite* site;
  int number;
  unsigned size;
  unsigned refCount;
  unsigned freezeCount;
  bool reserved;
};

class FrameResource {
 public:
  Value* value;
  MemorySite* site;
  unsigned size;
  unsigned freezeCount;
};

class ConstantPoolNode {
 public:
  ConstantPoolNode(Promise* promise): promise(promise), next(0) { }

  Promise* promise;
  ConstantPoolNode* next;
};

class Read {
 public:
  Read(unsigned size):
    value(0), event(0), eventNext(0), size(size)
  { }

  virtual ~Read() { }

  virtual Site* pickSite(Context* c, Value* v) = 0;

  virtual Site* allocateSite(Context* c) = 0;

  virtual bool intersect(uint8_t* typeMask, uint64_t* registerMask,
                         int* frameIndex) = 0;
  
  virtual bool valid() = 0;

  virtual void append(Context* c, Read* r) = 0;

  virtual Read* next(Context* c) = 0;

  Value* value;
  Event* event;
  Read* eventNext;
  unsigned size;
};

int
intersectFrameIndexes(int a, int b)
{
  if (a == NoFrameIndex or b == NoFrameIndex) return NoFrameIndex;
  if (a == AnyFrameIndex) return b;
  if (b == AnyFrameIndex) return a;
  if (a == b) return a;
  return NoFrameIndex;
}

class Value: public Compiler::Operand {
 public:
  Value(Site* site, Site* target):
    reads(0), lastRead(0), sites(site), source(0), target(target),
    visited(false)
  { }

  virtual ~Value() { }

  virtual void addPredecessor(Context*, Event*) { }
  
  Read* reads;
  Read* lastRead;
  Site* sites;
  Site* source;
  Site* target;
  bool visited;
};

enum Pass {
  ScanPass,
  CompilePass
};

class Context {
 public:
  Context(System* system, Assembler* assembler, Zone* zone,
          Compiler::Client* client):
    system(system),
    assembler(assembler),
    arch(assembler->arch()),
    zone(zone),
    client(client),
    stack(0),
    locals(0),
    predecessor(0),
    logicalCode(0),
    registers
    (static_cast<Register**>
     (zone->allocate(sizeof(Register*) * arch->registerCount()))),
    frameResources(0),
    firstConstant(0),
    lastConstant(0),
    machineCode(0),
    firstEvent(0),
    lastEvent(0),
    state(0),
    logicalIp(-1),
    constantCount(0),
    logicalCodeLength(0),
    parameterFootprint(0),
    localFootprint(0),
    stackPadding(0),
    machineCodeSize(0),
    alignedFrameSize(0),
    availableRegisterCount(arch->registerCount()),
    constantCompare(CompareNone),
    pass(ScanPass)
  {
    for (unsigned i = 0; i < arch->registerCount(); ++i) {
      registers[i] = new (zone->allocate(sizeof(Register))) Register(i);
      if (arch->reserved(i)) {
        registers[i]->reserved = true;
        -- availableRegisterCount;
      }
    }
  }

  System* system;
  Assembler* assembler;
  Assembler::Architecture* arch;
  Zone* zone;
  Compiler::Client* client;
  Stack* stack;
  Local* locals;
  Event* predecessor;
  LogicalInstruction** logicalCode;
  Register** registers;
  FrameResource* frameResources;
  ConstantPoolNode* firstConstant;
  ConstantPoolNode* lastConstant;
  uint8_t* machineCode;
  Event* firstEvent;
  Event* lastEvent;
  MyState* state;
  int logicalIp;
  unsigned constantCount;
  unsigned logicalCodeLength;
  unsigned parameterFootprint;
  unsigned localFootprint;
  unsigned stackPadding;
  unsigned machineCodeSize;
  unsigned alignedFrameSize;
  unsigned availableRegisterCount;
  ConstantCompare constantCompare;
  Pass pass;
};

class PoolPromise: public Promise {
 public:
  PoolPromise(Context* c, int key): c(c), key(key) { }

  virtual int64_t value() {
    if (resolved()) {
      return reinterpret_cast<intptr_t>
        (c->machineCode + pad(c->machineCodeSize) + (key * BytesPerWord));
    }
    
    abort(c);
  }

  virtual bool resolved() {
    return c->machineCode != 0;
  }

  Context* c;
  int key;
};

class CodePromise: public Promise {
 public:
  CodePromise(Context* c, CodePromise* next):
    c(c), offset(0), next(next)
  { }

  CodePromise(Context* c, Promise* offset):
    c(c), offset(offset), next(0)
  { }

  virtual int64_t value() {
    if (resolved()) {
      return reinterpret_cast<intptr_t>(c->machineCode + offset->value());
    }
    
    abort(c);
  }

  virtual bool resolved() {
    return c->machineCode != 0 and offset and offset->resolved();
  }

  Context* c;
  Promise* offset;
  CodePromise* next;
};

unsigned
machineOffset(Context* c, int logicalIp)
{
  return c->logicalCode[logicalIp]->machineOffset->value();
}

class IpPromise: public Promise {
 public:
  IpPromise(Context* c, int logicalIp):
    c(c),
    logicalIp(logicalIp)
  { }

  virtual int64_t value() {
    if (resolved()) {
      return reinterpret_cast<intptr_t>
        (c->machineCode + machineOffset(c, logicalIp));
    }

    abort(c);
  }

  virtual bool resolved() {
    return c->machineCode != 0;
  }

  Context* c;
  int logicalIp;
};

inline void NO_RETURN
abort(Context* c)
{
  abort(c->system);
}

#ifndef NDEBUG
inline void
assert(Context* c, bool v)
{
  assert(c->system, v);
}
#endif // not NDEBUG

inline void
expect(Context* c, bool v)
{
  expect(c->system, v);
}

Cell*
cons(Context* c, void* value, Cell* next)
{
  return new (c->zone->allocate(sizeof(Cell))) Cell(next, value);
}

Cell*
append(Context* c, Cell* first, Cell* second)
{
  if (first) {
    if (second) {
      Cell* start = cons(c, first->value, second);
      Cell* end = start;
      for (Cell* cell = first->next; cell; cell = cell->next) {
        Cell* n = cons(c, cell->value, second);
        end->next = n;
        end = n;
      }
      return start;
    } else {
      return first;
    }
  } else {
    return second;
  }
}

unsigned
count(Cell* c)
{
  unsigned n = 0;
  for (; c; c = c->next) ++ n;
  return n;
}

class StubReadPair {
 public:
  Value* value;
  StubRead* read;
};

class Event {
 public:
  Event(Context* c):
    next(0), stackBefore(c->stack), localsBefore(c->locals),
    stackAfter(0), localsAfter(0), promises(0), reads(0),
    junctionSites(0), savedSites(0), predecessors(0), successors(0), block(0),
    logicalInstruction(c->logicalCode[c->logicalIp]), state(c->state),
    junctionReads(0), readCount(0)
  {
    assert(c, c->logicalIp >= 0);

    if (c->lastEvent) {
      c->lastEvent->next = this;
    } else {
      c->firstEvent = this;
    }
    c->lastEvent = this;

    Event* p = c->predecessor;
    if (p) {
      p->stackAfter = stackBefore;
      p->localsAfter = localsBefore;

      predecessors = cons(c, p, 0);
      p->successors = cons(c, this, p->successors);
    }

    c->predecessor = this;

    if (logicalInstruction->firstEvent == 0) {
      logicalInstruction->firstEvent = this;
    }
    logicalInstruction->lastEvent = this;

    c->state = 0;
  }

  virtual ~Event() { }

  virtual const char* name() = 0;
  virtual void compile(Context* c) = 0;

  virtual void compilePostsync(Context*) { }

  Event* next;
  Stack* stackBefore;
  Local* localsBefore;
  Stack* stackAfter;
  Local* localsAfter;
  CodePromise* promises;
  Read* reads;
  Site** junctionSites;
  Site** savedSites;
  Cell* predecessors;
  Cell* successors;
  Block* block;
  LogicalInstruction* logicalInstruction;
  MyState* state;
  StubReadPair* junctionReads;
  unsigned readCount;
};

int
localOffset(Context* c, int frameIndex)
{
  int parameterFootprint = c->parameterFootprint;
  int frameSize = c->alignedFrameSize;

  int offset = ((frameIndex < parameterFootprint) ?
                (frameSize
                 + parameterFootprint
                 + (c->arch->frameFooterSize() * 2)
                 + c->arch->frameHeaderSize()
                 - frameIndex - 1) :
                (frameSize
                 + parameterFootprint
                 + c->arch->frameFooterSize()
                 - frameIndex - 1)) * BytesPerWord;

  assert(c, offset >= 0);

  return offset;
}

int
localOffsetToFrameIndex(Context* c, int offset)
{
  int parameterFootprint = c->parameterFootprint;
  int frameSize = c->alignedFrameSize;

  int normalizedOffset = offset / BytesPerWord;

  int frameIndex = ((normalizedOffset > frameSize) ?
                    (frameSize
                     + parameterFootprint
                     + (c->arch->frameFooterSize() * 2)
                     + c->arch->frameHeaderSize()
                     - normalizedOffset - 1) :
                    (frameSize
                     + parameterFootprint
                     + c->arch->frameFooterSize()
                     - normalizedOffset - 1));

  assert(c, frameIndex >= 0);
  assert(c, localOffset(c, frameIndex) == offset);

  return frameIndex;
}

bool
findSite(Context*, Value* v, Site* site)
{
  for (Site* s = v->sites; s; s = s->next) {
    if (s == site) return true;
  }
  return false;
}

void
addSite(Context* c, Stack* stack, Local* locals, unsigned size, Value* v,
        Site* s)
{
  if (not findSite(c, v, s)) {
//     fprintf(stderr, "add site %p (%d) to %p\n", s, s->type(c), v);
    s->acquire(c, stack, locals, size, v);
    s->next = v->sites;
    v->sites = s;
  }
}

void
removeSite(Context* c, Value* v, Site* s)
{
  for (Site** p = &(v->sites); *p;) {
    if (s == *p) {
//       fprintf(stderr, "remove site %p (%d) from %p\n", s, s->type(c), v);
      s->release(c);
      *p = (*p)->next;
      break;
    } else {
      p = &((*p)->next);
    }
  }
}

void
removeMemorySites(Context* c, Value* v)
{
  for (Site** p = &(v->sites); *p;) {
    if ((*p)->type(c) == MemoryOperand) {
//       fprintf(stderr, "remove site %p (%d) from %p\n", *p, (*p)->type(c), v);
      (*p)->release(c);
      *p = (*p)->next;
      break;
    } else {
      p = &((*p)->next);
    }
  }
}

void
clearSites(Context* c, Value* v)
{
  //fprintf(stderr, "clear sites for %p\n", v);
  for (Site* s = v->sites; s; s = s->next) {
    s->release(c);
  }
  v->sites = 0;
}

bool
valid(Read* r)
{
  return r and r->valid();
}

bool
live(Value* v)
{
  return valid(v->reads);
}

void
nextRead(Context* c, Event* e, Value* v)
{
  assert(c, e == v->reads->event);

//   fprintf(stderr, "pop read %p from %p; next: %p\n",
//           v->reads, v, v->reads->next(c));

  v->reads = v->reads->next(c);
  if (not live(v)) {
    clearSites(c, v);
  }
}

class ConstantSite: public Site {
 public:
  ConstantSite(Promise* value): value(value) { }

  virtual void toString(Context*, char* buffer, unsigned bufferSize) {
    if (value.value->resolved()) {
      snprintf(buffer, bufferSize, "constant %"LLD, value.value->value());
    } else {
      snprintf(buffer, bufferSize, "constant unresolved");
    }
  }

  virtual unsigned copyCost(Context*, Site* s) {
    return (s == this ? 0 : 1);
  }

  virtual bool match(Context*, uint8_t typeMask, uint64_t, int) {
    return typeMask & (1 << ConstantOperand);
  }

  virtual OperandType type(Context*) {
    return ConstantOperand;
  }

  virtual Assembler::Operand* asAssemblerOperand(Context*) {
    return &value;
  }

  Assembler::Constant value;
};

ConstantSite*
constantSite(Context* c, Promise* value)
{
  return new (c->zone->allocate(sizeof(ConstantSite))) ConstantSite(value);
}

ResolvedPromise*
resolved(Context* c, int64_t value)
{
  return new (c->zone->allocate(sizeof(ResolvedPromise)))
    ResolvedPromise(value);
}

ConstantSite*
constantSite(Context* c, int64_t value)
{
  return constantSite(c, resolved(c, value));
}

class AddressSite: public Site {
 public:
  AddressSite(Promise* address): address(address) { }

  virtual void toString(Context*, char* buffer, unsigned bufferSize) {
    if (address.address->resolved()) {
      snprintf(buffer, bufferSize, "address %"LLD, address.address->value());
    } else {
      snprintf(buffer, bufferSize, "address unresolved");
    }
  }

  virtual unsigned copyCost(Context*, Site* s) {
    return (s == this ? 0 : 3);
  }

  virtual bool match(Context*, uint8_t typeMask, uint64_t, int) {
    return typeMask & (1 << AddressOperand);
  }

  virtual OperandType type(Context*) {
    return AddressOperand;
  }

  virtual Assembler::Operand* asAssemblerOperand(Context*) {
    return &address;
  }

  Assembler::Address address;
};

AddressSite*
addressSite(Context* c, Promise* address)
{
  return new (c->zone->allocate(sizeof(AddressSite))) AddressSite(address);
}

void
freeze(Context* c, Register* r)
{
  assert(c, c->availableRegisterCount);

  if (DebugRegisters) {
    fprintf(stderr, "freeze %d to %d\n", r->number, r->freezeCount + 1);
  }

  ++ r->freezeCount;
  -- c->availableRegisterCount;
}

void
thaw(Context* c, Register* r)
{
  assert(c, r->freezeCount);

  if (DebugRegisters) {
    fprintf(stderr, "thaw %d to %d\n", r->number, r->freezeCount - 1);
  }

  -- r->freezeCount;
  ++ c->availableRegisterCount;
}

Register*
acquire(Context* c, uint32_t mask, Stack* stack, Local* locals,
        unsigned newSize, Value* newValue, RegisterSite* newSite);

void
release(Context* c, Register* r);

Register*
validate(Context* c, uint32_t mask, Stack* stack, Local* locals,
         unsigned size, Value* value, RegisterSite* site, Register* current);

class RegisterSite: public Site {
 public:
  RegisterSite(uint64_t mask, Register* low = 0, Register* high = 0):
    mask(mask), low(low), high(high), register_(NoRegister, NoRegister)
  { }

  void sync(Context* c UNUSED) {
    assert(c, low);

    register_.low = low->number;
    register_.high = (high? high->number : NoRegister);
  }

  virtual void toString(Context* c, char* buffer, unsigned bufferSize) {
    if (low) {
      sync(c);

      snprintf(buffer, bufferSize, "register %d %d",
               register_.low, register_.high);
    } else {
      snprintf(buffer, bufferSize, "register unacquired");
    }
  }

  virtual unsigned copyCost(Context* c, Site* s) {
    sync(c);

    if (s and
        (this == s or
         (s->type(c) == RegisterOperand
          and (static_cast<RegisterSite*>(s)->mask
               & (static_cast<uint64_t>(1) << register_.low))
          and (register_.high == NoRegister
               or (static_cast<RegisterSite*>(s)->mask
                   & (static_cast<uint64_t>(1) << (register_.high + 32)))))))
    {
      return 0;
    } else {
      return 2;
    }
  }

  virtual bool match(Context* c, uint8_t typeMask, uint64_t registerMask, int)
  {
    if ((typeMask & (1 << RegisterOperand)) and low) {
      sync(c);
      return ((static_cast<uint64_t>(1) << register_.low) & registerMask)
        and (register_.high == NoRegister
             or ((static_cast<uint64_t>(1) << (register_.high + 32))
                 & registerMask));
    } else {
      return false;
    }
  }

  virtual void acquire(Context* c, Stack* stack, Local* locals, unsigned size,
                       Value* v)
  {
    low = ::validate(c, mask, stack, locals, size, v, this, low);
    if (size > BytesPerWord) {
      ::freeze(c, low);
      high = ::validate(c, mask >> 32, stack, locals, size, v, this, high);
      ::thaw(c, low);
    }
  }

  virtual void release(Context* c) {
    assert(c, low);

    ::release(c, low);
    if (high) {
      ::release(c, high);
    }
  }

  virtual void freeze(Context* c UNUSED) {
    assert(c, low);

    ::freeze(c, low);
    if (high) {
      ::freeze(c, high);
    }
  }

  virtual void thaw(Context* c UNUSED) {
    assert(c, low);

    ::thaw(c, low);
    if (high) {
      ::thaw(c, high);
    }
  }

  virtual OperandType type(Context*) {
    return RegisterOperand;
  }

  virtual Assembler::Operand* asAssemblerOperand(Context* c) {
    sync(c);
    return &register_;
  }

  virtual void makeSpecific(Context* c UNUSED) {
    assert(c, low);

    mask = static_cast<uint64_t>(1) << low->number;
    if (high) {
      mask |= static_cast<uint64_t>(1) << (high->number + 32);
    }
  }

  uint64_t mask;
  Register* low;
  Register* high;
  Assembler::Register register_;
};

RegisterSite*
registerSite(Context* c, int low, int high = NoRegister)
{
  assert(c, low != NoRegister);
  assert(c, low < static_cast<int>(c->arch->registerCount()));
  assert(c, high == NoRegister
         or high < static_cast<int>(c->arch->registerCount()));

  Register* hr;
  if (high == NoRegister) {
    hr = 0;
  } else {
    hr = c->registers[high];
  }
  return new (c->zone->allocate(sizeof(RegisterSite)))
    RegisterSite(~static_cast<uint64_t>(0), c->registers[low], hr);
}

RegisterSite*
freeRegisterSite(Context* c, uint64_t mask = ~static_cast<uint64_t>(0))
{
  return new (c->zone->allocate(sizeof(RegisterSite)))
    RegisterSite(mask);
}

Register*
increment(Context* c, int i)
{
  Register* r = c->registers[i];

  if (DebugRegisters) {
    fprintf(stderr, "increment %d to %d\n", r->number, r->refCount + 1);
  }

  ++ r->refCount;

  return r;
}

void
decrement(Context* c UNUSED, Register* r)
{
  assert(c, r->refCount > 0);

  if (DebugRegisters) {
    fprintf(stderr, "decrement %d to %d\n", r->number, r->refCount - 1);
  }

  -- r->refCount;
}

void
acquireFrameIndex(Context* c, int index, Stack* stack, Local* locals,
                  unsigned newSize, Value* newValue, MemorySite* newSite,
                  bool recurse = true);

void
releaseFrameIndex(Context* c, int index, bool recurse = true);

class MemorySite: public Site {
 public:
  MemorySite(int base, int offset, int index, unsigned scale):
    base(0), index(0), value(base, offset, index, scale)
  { }

  void sync(Context* c UNUSED) {
    assert(c, base);

    value.base = base->number;
    value.index = (index? index->number : NoRegister);
  }

  virtual void toString(Context* c, char* buffer, unsigned bufferSize) {
    if (base) {
      sync(c);

      snprintf(buffer, bufferSize, "memory %d %d %d %d",
               value.base, value.offset, value.index, value.scale);
    } else {
      snprintf(buffer, bufferSize, "memory unacquired");
    }
  }

  virtual unsigned copyCost(Context* c, Site* s) {
    sync(c);

    if (s and
        (this == s or
         (s->type(c) == MemoryOperand
          and static_cast<MemorySite*>(s)->value.base == value.base
          and static_cast<MemorySite*>(s)->value.offset == value.offset
          and static_cast<MemorySite*>(s)->value.index == value.index
          and static_cast<MemorySite*>(s)->value.scale == value.scale)))
    {
      return 0;
    } else {
      return 4;
    }
  }

  virtual bool match(Context* c, uint8_t typeMask, uint64_t, int frameIndex) {
    if (typeMask & (1 << MemoryOperand)) {
      sync(c);
      if (value.base == c->arch->stack()) {
        assert(c, value.index == NoRegister);
        return frameIndex == AnyFrameIndex
          || (frameIndex != NoFrameIndex
              && localOffset(c, frameIndex) == value.offset);
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  virtual void acquire(Context* c, Stack* stack, Local* locals, unsigned size,
                       Value* v)
  {
    base = increment(c, value.base);
    if (value.index != NoRegister) {
      index = increment(c, value.index);
    }

    if (value.base == c->arch->stack()) {
      assert(c, value.index == NoRegister);
      acquireFrameIndex
        (c, localOffsetToFrameIndex(c, value.offset), stack, locals, size, v,
         this);
    }
  }

  virtual void release(Context* c) {
    if (value.base == c->arch->stack()) {
      assert(c, value.index == NoRegister);
      releaseFrameIndex(c, localOffsetToFrameIndex(c, value.offset));
    }

    decrement(c, base);
    if (index) {
      decrement(c, index);
    }
  }

  virtual OperandType type(Context*) {
    return MemoryOperand;
  }

  virtual Assembler::Operand* asAssemblerOperand(Context* c) {
    sync(c);
    return &value;
  }

  Register* base;
  Register* index;
  Assembler::Memory value;
};

MemorySite*
memorySite(Context* c, int base, int offset = 0, int index = NoRegister,
           unsigned scale = 1)
{
  return new (c->zone->allocate(sizeof(MemorySite)))
    MemorySite(base, offset, index, scale);
}

MemorySite*
frameSite(Context* c, int frameIndex)
{
  assert(c, frameIndex >= 0);
  return memorySite(c, c->arch->stack(), localOffset(c, frameIndex));
}

Site*
targetOrNull(Context* c, Value* v, Read* r)
{
  if (v->target) {
    return v->target;
  } else {
    Site* s = r->pickSite(c, v);
    if (s) return s;
    return r->allocateSite(c);
  }
}

Site*
targetOrNull(Context* c, Value* v)
{
  if (v->target) {
    return v->target;
  } else if (live(v)) {
    Read* r = v->reads;
    Site* s = r->pickSite(c, v);
    if (s) return s;
    return r->allocateSite(c);
  }
  return 0;
}

Site*
pickSite(Context* c, Value* value, uint8_t typeMask, uint64_t registerMask,
         int frameIndex)
{
  Site* site = 0;
  unsigned copyCost = 0xFFFFFFFF;
  for (Site* s = value->sites; s; s = s->next) {
    if (s->match(c, typeMask, registerMask, frameIndex)) {
      unsigned v = s->copyCost(c, 0);
      if (v < copyCost) {
        site = s;
        copyCost = v;
      }
    }
  }
  return site;
}

Site*
allocateSite(Context* c, uint8_t typeMask, uint64_t registerMask,
             int frameIndex)
{
  if ((typeMask & (1 << RegisterOperand)) and registerMask) {
    return freeRegisterSite(c, registerMask);
  } else if (frameIndex >= 0) {
    return frameSite(c, frameIndex);
  } else {
    return 0;
  }
}

class SingleRead: public Read {
 public:
  SingleRead(unsigned size, uint8_t typeMask, uint64_t registerMask,
             int frameIndex):
    Read(size), next_(0), typeMask(typeMask), registerMask(registerMask),
    frameIndex(frameIndex)
  { }

  virtual Site* pickSite(Context* c, Value* value) {
    return ::pickSite(c, value, typeMask, registerMask, frameIndex);
  }

  virtual Site* allocateSite(Context* c) {
    return ::allocateSite(c, typeMask, registerMask, frameIndex);
  }

  virtual bool intersect(uint8_t* typeMask, uint64_t* registerMask,
                         int* frameIndex)
  {
    *typeMask &= this->typeMask;
    *registerMask &= this->registerMask;
    *frameIndex = intersectFrameIndexes(*frameIndex, this->frameIndex);
    return true;
  }
  
  virtual bool valid() {
    return true;
  }

  virtual void append(Context* c, Read* r) {
    assert(c, next_ == 0);
    next_ = r;
  }

  virtual Read* next(Context*) {
    return next_;
  }

  Read* next_;
  uint8_t typeMask;
  uint64_t registerMask;
  int frameIndex;
};

Read*
read(Context* c, unsigned size, uint8_t typeMask, uint64_t registerMask,
     int frameIndex)
{
  assert(c, (typeMask != 1 << MemoryOperand) or frameIndex >= 0);
  return new (c->zone->allocate(sizeof(SingleRead)))
    SingleRead(size, typeMask, registerMask, frameIndex);
}

Read*
anyRegisterRead(Context* c, unsigned size)
{
  return read(c, size, 1 << RegisterOperand, ~static_cast<uint64_t>(0),
              NoFrameIndex);
}

Read*
registerOrConstantRead(Context* c, unsigned size)
{
  return read(c, size, (1 << RegisterOperand) | (1 << ConstantOperand),
              ~static_cast<uint64_t>(0), NoFrameIndex);
}

Read*
fixedRegisterRead(Context* c, unsigned size, int low, int high = NoRegister)
{
  uint64_t mask;
  if (high == NoRegister) {
    mask = (~static_cast<uint64_t>(0) << 32)
      | (static_cast<uint64_t>(1) << low);
  } else {
    mask = (static_cast<uint64_t>(1) << (high + 32))
      | (static_cast<uint64_t>(1) << low);
  }

  return read(c, size, 1 << RegisterOperand, mask, NoFrameIndex);
}

class MultiRead: public Read {
 public:
  MultiRead(unsigned size):
    Read(size), reads(0), lastRead(0), firstTarget(0), lastTarget(0),
    visited(false)
  { }

  virtual Site* pickSite(Context* c, Value* value) {
    uint8_t typeMask = ~static_cast<uint8_t>(0);
    uint64_t registerMask = ~static_cast<uint64_t>(0);
    int frameIndex = AnyFrameIndex;
    intersect(&typeMask, &registerMask, &frameIndex);

    return ::pickSite(c, value, typeMask, registerMask, frameIndex);
  }

  virtual Site* allocateSite(Context* c) {
    uint8_t typeMask = ~static_cast<uint8_t>(0);
    uint64_t registerMask = ~static_cast<uint64_t>(0);
    int frameIndex = AnyFrameIndex;
    intersect(&typeMask, &registerMask, &frameIndex);

    return ::allocateSite(c, typeMask, registerMask, frameIndex);
  }

  virtual bool intersect(uint8_t* typeMask, uint64_t* registerMask,
                         int* frameIndex)
  {
    bool result = false;
    if (not visited) {
      visited = true;
      for (Cell** cell = &reads; *cell;) {
        Read* r = static_cast<Read*>((*cell)->value);
        bool valid = r->intersect(typeMask, registerMask, frameIndex);
        if (valid) {
          result = true;
          cell = &((*cell)->next);
        } else {
          *cell = (*cell)->next;
        }
      }
      visited = false;
    }
    return result;
  }

  virtual bool valid() {
    bool result = false;
    if (not visited) {
      visited = true;
      for (Cell** cell = &reads; *cell;) {
        Read* r = static_cast<Read*>((*cell)->value);
        if (r->valid()) {
          result = true;
          cell = &((*cell)->next);
        } else {
          *cell = (*cell)->next;
        }
      }
      visited = false;
    }
    return result;
  }

  virtual void append(Context* c, Read* r) {
    Cell* cell = cons(c, r, 0);
    if (lastRead == 0) {
      reads = cell;
    } else {
      lastRead->next = cell;
    }
    lastRead = cell;

    lastTarget->value = r;
  }

  virtual Read* next(Context* c) {
    abort(c);
  }

  void allocateTarget(Context* c) {
    Cell* cell = cons(c, 0, 0);
    if (lastTarget == 0) {
      firstTarget = cell;
    } else {
      lastTarget->next = cell;
    }
    lastTarget = cell;
  }

  Read* nextTarget() {
    Read* r = static_cast<Read*>(firstTarget->value);
    firstTarget = firstTarget->next;
    return r;
  }

  Cell* reads;
  Cell* lastRead;
  Cell* firstTarget;
  Cell* lastTarget;
  bool visited;
};

MultiRead*
multiRead(Context* c, unsigned size)
{
  return new (c->zone->allocate(sizeof(MultiRead))) MultiRead(size);
}

class StubRead: public Read {
 public:
  StubRead(unsigned size):
    Read(size), read(0), visited(false)
  { }

  virtual Site* pickSite(Context* c, Value* value) {
    uint8_t typeMask = ~static_cast<uint8_t>(0);
    uint64_t registerMask = ~static_cast<uint64_t>(0);
    int frameIndex = AnyFrameIndex;
    intersect(&typeMask, &registerMask, &frameIndex);

    return ::pickSite(c, value, typeMask, registerMask, frameIndex);
  }

  virtual Site* allocateSite(Context* c) {
    uint8_t typeMask = ~static_cast<uint8_t>(0);
    uint64_t registerMask = ~static_cast<uint64_t>(0);
    int frameIndex = AnyFrameIndex;
    intersect(&typeMask, &registerMask, &frameIndex);

    return ::allocateSite(c, typeMask, registerMask, frameIndex);
  }

  virtual bool intersect(uint8_t* typeMask, uint64_t* registerMask,
                         int* frameIndex)
  {
    if (not visited) {
      visited = true;
      if (read) {
        bool valid = read->intersect(typeMask, registerMask, frameIndex);
        if (not valid) {
          read = 0;
        }
      }
      visited = false;
    }
    return true;
  }

  virtual bool valid() {
    return true;
  }

  virtual void append(Context*, Read* r) {
    read = r;
  }

  virtual Read* next(Context* c) {
    abort(c);
  }

  Read* read;
  bool visited;
};

StubRead*
stubRead(Context* c, unsigned size)
{
  return new (c->zone->allocate(sizeof(StubRead))) StubRead(size);
}

Site*
targetOrRegister(Context* c, Value* v)
{
  Site* s = targetOrNull(c, v);
  if (s) {
    return s;
  } else {
    return freeRegisterSite(c);
  }
}

Site*
pick(Context* c, Site* sites, Site* target = 0, unsigned* cost = 0)
{
  Site* site = 0;
  unsigned copyCost = 0xFFFFFFFF;
  for (Site* s = sites; s; s = s->next) {
    unsigned v = s->copyCost(c, target);
    if (v < copyCost) {
      site = s;
      copyCost = v;
    }
  }

  if (cost) *cost = copyCost;
  return site;
}

bool
trySteal(Context* c, Register* r, Stack* stack, Local* locals)
{
  assert(c, r->refCount == 0);

  Value* v = r->value;
  assert(c, v->reads);

  if (DebugRegisters) {
    fprintf(stderr, "try steal %d from %p: next: %p\n",
            r->number, v, v->sites->next);
  }

  if (v->sites->next == 0) {
    Site* saveSite = 0;
    for (unsigned i = 0; i < c->localFootprint; ++i) {
      if (locals[i].value == v) {
        saveSite = frameSite(c, i);
        break;
      }
    }

    if (saveSite == 0) {
      for (Stack* s = stack; s; s = s->next) {
        if (s->value == v) {
          uint8_t typeMask;
          uint64_t registerMask;
          int frameIndex = AnyFrameIndex;
          v->reads->intersect(&typeMask, &registerMask, &frameIndex);

          if (frameIndex >= 0) {
            saveSite = frameSite(c, frameIndex);
          } else {
            saveSite = frameSite(c, s->index + c->localFootprint);
          }
          break;
        }
      }
    }

    if (saveSite) {
      addSite(c, 0, 0, r->size, v, saveSite);
      apply(c, Move, r->size, r->site, r->size, saveSite);
    } else {
      if (DebugRegisters) {
        fprintf(stderr, "unable to steal %d from %p\n", r->number, v);
      }
      return false;
    }
  }

  removeSite(c, v, r->site);

  return true;
}

bool
used(Context* c, Register* r)
{
  Value* v = r->value;
  return v and findSite(c, v, r->site);
}

bool
usedExclusively(Context* c, Register* r)
{
  return used(c, r) and r->value->sites->next == 0;
}

unsigned
registerCost(Context* c, Register* r)
{
  if (r->reserved or r->freezeCount) {
    return 6;
  }

  unsigned cost = 0;

  if (used(c, r)) {
    ++ cost;
    if (usedExclusively(c, r)) {
      cost += 2;
    }
  }

  if (r->refCount) {
    cost += 2;
  }

  return cost;
}

Register*
pickRegister(Context* c, uint32_t mask)
{
  Register* register_ = 0;
  unsigned cost = 5;
  for (int i = c->arch->registerCount() - 1; i >= 0; --i) {
    if ((1 << i) & mask) {
      Register* r = c->registers[i];
      if ((static_cast<uint32_t>(1) << i) == mask) {
        return r;
      }

      unsigned myCost = registerCost(c, r);
      if (myCost < cost) {
        register_ = r;
        cost = myCost;
      }
    }
  }

  expect(c, register_);

  return register_;
}

void
swap(Context* c, Register* a, Register* b)
{
  assert(c, a != b);
  assert(c, a->number != b->number);

  Assembler::Register ar(a->number);
  Assembler::Register br(b->number);
  c->assembler->apply
    (Swap, BytesPerWord, RegisterOperand, &ar,
     BytesPerWord, RegisterOperand, &br);
  
  c->registers[a->number] = b;
  c->registers[b->number] = a;

  int t = a->number;
  a->number = b->number;
  b->number = t;
}

Register*
replace(Context* c, Stack* stack, Local* locals, Register* r)
{
  uint32_t mask = (r->freezeCount? r->site->mask : ~0);

  freeze(c, r);
  Register* s = acquire(c, mask, stack, locals, r->size, r->value, r->site);
  thaw(c, r);

  if (DebugRegisters) {
    fprintf(stderr, "replace %d with %d\n", r->number, s->number);
  }

  swap(c, r, s);

  return s;
}

Register*
acquire(Context* c, uint32_t mask, Stack* stack, Local* locals,
        unsigned newSize, Value* newValue, RegisterSite* newSite)
{
  Register* r = pickRegister(c, mask);

  if (r->reserved) return r;

  if (DebugRegisters) {
    fprintf(stderr, "acquire %d value %p site %p freeze count %d "
            "ref count %d used %d used exclusively %d\n",
            r->number, newValue, newSite, r->freezeCount, r->refCount,
            used(c, r), usedExclusively(c, r));
  }

  if (r->refCount) {
    r = replace(c, stack, locals, r);
  } else {
    Value* oldValue = r->value;
    if (oldValue
        and oldValue != newValue
        and findSite(c, oldValue, r->site))
    {
      if (not trySteal(c, r, stack, locals)) {
        r = replace(c, stack, locals, r);
      }
    }
  }

  r->size = newSize;
  r->value = newValue;
  r->site = newSite;

  return r;
}

void
release(Context*, Register* r)
{
  if (DebugRegisters) {
    fprintf(stderr, "release %d\n", r->number);
  }

  r->size = 0;
  r->value = 0;
  r->site = 0;  
}

Register*
validate(Context* c, uint32_t mask, Stack* stack, Local* locals,
         unsigned size, Value* value, RegisterSite* site, Register* current)
{
  if (current and (mask & (1 << current->number))) {
    if (current->reserved or current->value == value) {
      return current;
    }

    if (current->value == 0) {
      if (DebugRegisters) {
        fprintf(stderr,
                "validate acquire %d value %p site %p freeze count %d "
                "ref count %d\n",
                current->number, value, site, current->freezeCount,
                current->refCount);
      }

      current->size = size;
      current->value = value;
      current->site = site;
      return current;
    } else {
      removeSite(c, current->value, current->site);
    }
  }

  Register* r = acquire(c, mask, stack, locals, size, value, site);

  if (current and current != r) {
    release(c, current);
    
    Assembler::Register rr(r->number);
    Assembler::Register cr(current->number);
    c->assembler->apply
      (Move, BytesPerWord, RegisterOperand, &cr,
       BytesPerWord, RegisterOperand, &rr);
  }

  return r;
}

bool
trySteal(Context* c, FrameResource* r, Stack*, Local*)
{
  Value* v = r->value;
  assert(c, v->reads);

//   if (v->sites->next == 0) {
//     return false; // todo
//   }

  if (DebugFrameIndexes) {
    int index = r - c->frameResources;
    fprintf(stderr,
            "steal frame index %d offset 0x%x from value %p site %p\n",
            index, localOffset(c, index), r->value, r->site);
  }

  removeSite(c, v, r->site);

  return true;
}

void
acquireFrameIndex(Context* c, int index, Stack* stack, Local* locals,
                  unsigned newSize, Value* newValue, MemorySite* newSite,
                  bool recurse)
{
  assert(c, index >= 0);
  assert(c, index < static_cast<int>
         (c->alignedFrameSize + c->parameterFootprint));

  if (DebugFrameIndexes) {
    fprintf(stderr,
            "acquire frame index %d offset 0x%x value %p site %p\n",
            index, localOffset(c, index), newValue, newSite);
  }

  FrameResource* r = c->frameResources + index;

  if (recurse and newSize > BytesPerWord) {
    acquireFrameIndex
      (c, index + 1, stack, locals, newSize, newValue, newSite, false);
  }

  Value* oldValue = r->value;
  if (oldValue
      and oldValue != newValue
      and findSite(c, oldValue, r->site))
  {
    if (not trySteal(c, r, stack, locals)) {
      abort(c);
    }
  }

  r->size = newSize;
  r->value = newValue;
  r->site = newSite;
}

void
releaseFrameIndex(Context* c, int index, bool recurse)
{
  assert(c, index >= 0);
  assert(c, index < static_cast<int>
         (c->alignedFrameSize + c->parameterFootprint));

  if (DebugFrameIndexes) {
    fprintf(stderr, "release frame index %d offset 0x%x\n",
            index, localOffset(c, index));
  }

  FrameResource* r = c->frameResources + index;

  if (recurse and r->size > BytesPerWord) {
    releaseFrameIndex(c, index + 1, false);
  }

  r->size = 0;
  r->value = 0;
  r->site = 0;
}

void
apply(Context* c, UnaryOperation op,
      unsigned s1Size, Site* s1)
{
  OperandType s1Type = s1->type(c);
  Assembler::Operand* s1Operand = s1->asAssemblerOperand(c);

  c->assembler->apply(op, s1Size, s1Type, s1Operand);
}

void
apply(Context* c, BinaryOperation op,
      unsigned s1Size, Site* s1,
      unsigned s2Size, Site* s2)
{
  OperandType s1Type = s1->type(c);
  Assembler::Operand* s1Operand = s1->asAssemblerOperand(c);

  OperandType s2Type = s2->type(c);
  Assembler::Operand* s2Operand = s2->asAssemblerOperand(c);

  c->assembler->apply(op, s1Size, s1Type, s1Operand,
                      s2Size, s2Type, s2Operand);
}

void
apply(Context* c, TernaryOperation op,
      unsigned s1Size, Site* s1,
      unsigned s2Size, Site* s2,
      unsigned s3Size, Site* s3)
{
  OperandType s1Type = s1->type(c);
  Assembler::Operand* s1Operand = s1->asAssemblerOperand(c);

  OperandType s2Type = s2->type(c);
  Assembler::Operand* s2Operand = s2->asAssemblerOperand(c);

  OperandType s3Type = s3->type(c);
  Assembler::Operand* s3Operand = s3->asAssemblerOperand(c);

  c->assembler->apply(op, s1Size, s1Type, s1Operand,
                      s2Size, s2Type, s2Operand,
                      s3Size, s3Type, s3Operand);
}

void
addRead(Context* c, Event* e, Value* v, Read* r)
{
//   fprintf(stderr, "add read %p to %p\n", r, v);

  r->value = v;
  if (e) {
    r->event = e;
    r->eventNext = e->reads;
    e->reads = r;
    ++ e->readCount;
  }

  if (v->lastRead) {
    //fprintf(stderr, "append %p to %p for %p\n", r, v->lastRead, v);
    v->lastRead->append(c, r);
  } else {
    v->reads = r;
  }
  v->lastRead = r;
}

void
clean(Context* c, Value* v)
{
  for (Site** s = &(v->sites); *s;) {
    if ((*s)->match(c, 1 << MemoryOperand, 0, AnyFrameIndex)) {
      s = &((*s)->next);
    } else {
      (*s)->release(c);
      *s = (*s)->next;
    }
  }
}

void
clean(Context* c, Event* e, Stack* stack, Local* locals, Read* reads)
{
  for (unsigned i = 0; i < c->localFootprint; ++i) {
    if (locals[i].value) clean(c, locals[i].value);
  }

  for (Stack* s = stack; s; s = s->next) {
    clean(c, s->value);
  }

  for (Read* r = reads; r; r = r->eventNext) {
    nextRead(c, e, r->value);
  }  
}

CodePromise*
codePromise(Context* c, Event* e)
{
  return e->promises = new (c->zone->allocate(sizeof(CodePromise)))
    CodePromise(c, e->promises);
}

CodePromise*
codePromise(Context* c, Promise* offset)
{
  return new (c->zone->allocate(sizeof(CodePromise))) CodePromise(c, offset);
}

class CallEvent: public Event {
 public:
  CallEvent(Context* c, Value* address, unsigned flags,
            TraceHandler* traceHandler, Value* result, unsigned resultSize,
            Stack* argumentStack, unsigned argumentCount,
            unsigned stackArgumentFootprint):
    Event(c),
    address(address),
    traceHandler(traceHandler),
    result(result),
    flags(flags),
    resultSize(resultSize)
  {
    uint32_t mask = ~0;
    Stack* s = argumentStack;
    unsigned index = 0;
    unsigned frameIndex = c->alignedFrameSize + c->parameterFootprint;
    for (unsigned i = 0; i < argumentCount; ++i) {
      Read* target;
      if (index < c->arch->argumentRegisterCount()) {
        int r = c->arch->argumentRegister(index);
        target = fixedRegisterRead(c, s->size * BytesPerWord, r);
        mask &= ~(1 << r);
      } else {
        frameIndex -= s->size;
        target = read(c, s->size * BytesPerWord, 1 << MemoryOperand, 0,
                      frameIndex);
      }
      addRead(c, this, s->value, target);
      index += s->size;
      s = s->next;
    }

    addRead(c, this, address, read
            (c, BytesPerWord, ~0, (static_cast<uint64_t>(mask) << 32) | mask,
             AnyFrameIndex));

    int footprint = stackArgumentFootprint;
    for (Stack* s = stackBefore; s; s = s->next) {
      frameIndex -= s->size;
      if (footprint > 0) {
        addRead(c, this, s->value, read
                (c, s->size * BytesPerWord,
                 1 << MemoryOperand, 0, frameIndex));
      } else {
        unsigned index = s->index + c->localFootprint;
        if (footprint == 0) {
          assert(c, index <= frameIndex);
          s->padding = frameIndex - index;
        }
        addRead(c, this, s->value, read
                (c, s->size * BytesPerWord, 1 << MemoryOperand, 0, index));
      }
      footprint -= s->size;
    }

    for (unsigned i = 0; i < c->localFootprint; ++i) {
      Local* local = localsBefore + i;
      if (local->value) {
        addRead(c, this, local->value, read
                (c, local->size, 1 << MemoryOperand, 0, i));
      }
    }
  }

  virtual const char* name() {
    return "CallEvent";
  }

  virtual void compile(Context* c) {
    apply(c, (flags & Compiler::Aligned) ? AlignedCall : Call, BytesPerWord,
          address->source);

    if (traceHandler) {
      traceHandler->handleTrace(codePromise(c, c->assembler->offset()));
    }

    clean(c, this, stackBefore, localsBefore, reads);

    if (resultSize and live(result)) {
      addSite(c, 0, 0, resultSize, result, registerSite
              (c, c->arch->returnLow(),
               resultSize > BytesPerWord ?
               c->arch->returnHigh() : NoRegister));
    }
  }

  Value* address;
  TraceHandler* traceHandler;
  Value* result;
  unsigned flags;
  unsigned resultSize;
};

void
appendCall(Context* c, Value* address, unsigned flags,
           TraceHandler* traceHandler, Value* result, unsigned resultSize,
           Stack* argumentStack, unsigned argumentCount,
           unsigned stackArgumentFootprint)
{
  new (c->zone->allocate(sizeof(CallEvent)))
    CallEvent(c, address, flags, traceHandler, result,
              resultSize, argumentStack, argumentCount,
              stackArgumentFootprint);
}

class ReturnEvent: public Event {
 public:
  ReturnEvent(Context* c, unsigned size, Value* value):
    Event(c), value(value)
  {
    if (value) {
      addRead(c, this, value, fixedRegisterRead
              (c, size, c->arch->returnLow(),
               size > BytesPerWord ?
               c->arch->returnHigh() : NoRegister));
    }
  }

  virtual const char* name() {
    return "ReturnEvent";
  }

  virtual void compile(Context* c) {
    if (value) {
      nextRead(c, this, value);
    }

    c->assembler->popFrame();
    c->assembler->apply(Return);
  }

  Value* value;
};

void
appendReturn(Context* c, unsigned size, Value* value)
{
  new (c->zone->allocate(sizeof(ReturnEvent))) ReturnEvent(c, size, value);
}

class MoveEvent: public Event {
 public:
  MoveEvent(Context* c, BinaryOperation type, unsigned srcSize, Value* src,
            unsigned dstSize, Value* dst, Read* srcRead, Read* dstRead):
    Event(c), type(type), srcSize(srcSize), src(src), dstSize(dstSize),
    dst(dst), dstRead(dstRead)
  {
    addRead(c, this, src, srcRead);
  }

  virtual const char* name() {
    return "MoveEvent";
  }

  virtual void compile(Context* c) {
    bool isLoad = not valid(src->reads->next(c));
    bool isStore = not valid(dst->reads);

    Site* target = targetOrRegister(c, dst);
    unsigned cost = src->source->copyCost(c, target);
    if (cost == 0 and (isLoad or isStore)) {
      target = src->source;
    }

    assert(c, isLoad or isStore or target != src->source);

    if (target == src->source) {
      removeSite(c, src, target);
    }

    if (not isStore) {
      addSite(c, stackBefore, localsBefore, dstSize, dst, target);
    }

    if (cost or type != Move) {    
      uint8_t typeMask = ~static_cast<uint8_t>(0);
      uint64_t registerMask = ~static_cast<uint64_t>(0);
      int frameIndex = AnyFrameIndex;
      dstRead->intersect(&typeMask, &registerMask, &frameIndex);

      bool memoryToMemory = (target->type(c) == MemoryOperand
                             and src->source->type(c) == MemoryOperand);

      if (target->match(c, typeMask, registerMask, frameIndex)
          and not memoryToMemory)
      {
        apply(c, type, srcSize, src->source, dstSize, target);
      } else {
        assert(c, typeMask & (1 << RegisterOperand));

        Site* tmpTarget = freeRegisterSite(c, registerMask);

        addSite(c, stackBefore, localsBefore, dstSize, dst, tmpTarget);

        apply(c, type, srcSize, src->source, dstSize, tmpTarget);

        if (isStore) {
          removeSite(c, dst, tmpTarget);
        }

        if (memoryToMemory or isStore) {
          apply(c, Move, dstSize, tmpTarget, dstSize, target);
        } else {
          removeSite(c, dst, target);          
        }
      }
    }

    if (isStore) {
      removeSite(c, dst, target);
    }

    nextRead(c, this, src);
  }

  BinaryOperation type;
  unsigned srcSize;
  Value* src;
  unsigned dstSize;
  Value* dst;
  Read* dstRead;
};

void
appendMove(Context* c, BinaryOperation type, unsigned srcSize, Value* src,
           unsigned dstSize, Value* dst)
{
  bool thunk;
  uint8_t srcTypeMask;
  uint64_t srcRegisterMask;
  uint8_t dstTypeMask;
  uint64_t dstRegisterMask;

  c->arch->plan(type, srcSize, &srcTypeMask, &srcRegisterMask,
                dstSize, &dstTypeMask, &dstRegisterMask,
                &thunk);

  assert(c, not thunk); // todo

  new (c->zone->allocate(sizeof(MoveEvent)))
    MoveEvent(c, type, srcSize, src, dstSize, dst,
              read(c, srcSize, srcTypeMask, srcRegisterMask, AnyFrameIndex),
              read(c, dstSize, dstTypeMask, dstRegisterMask, AnyFrameIndex));
}

ConstantSite*
findConstantSite(Context* c, Value* v)
{
  for (Site* s = v->sites; s; s = s->next) {
    if (s->type(c) == ConstantOperand) {
      return static_cast<ConstantSite*>(s);
    }
  }
  return 0;
}

class CompareEvent: public Event {
 public:
  CompareEvent(Context* c, unsigned size, Value* first, Value* second,
               Read* firstRead, Read* secondRead):
    Event(c), size(size), first(first), second(second)
  {
    addRead(c, this, first, firstRead);
    addRead(c, this, second, secondRead);
  }

  virtual const char* name() {
    return "CompareEvent";
  }

  virtual void compile(Context* c) {
    ConstantSite* firstConstant = findConstantSite(c, first);
    ConstantSite* secondConstant = findConstantSite(c, second);

    if (firstConstant and secondConstant) {
      int64_t d = firstConstant->value.value->value()
        - secondConstant->value.value->value();

      if (d < 0) {
        c->constantCompare = CompareLess;
      } else if (d > 0) {
        c->constantCompare = CompareGreater;
      } else {
        c->constantCompare = CompareEqual;
      }
    } else {
      c->constantCompare = CompareNone;

      apply(c, Compare, size, first->source, size, second->source);
    }

    nextRead(c, this, first);
    nextRead(c, this, second);
  }

  unsigned size;
  Value* first;
  Value* second;
};

void
appendCompare(Context* c, unsigned size, Value* first, Value* second)
{
  bool thunk;
  uint8_t firstTypeMask;
  uint64_t firstRegisterMask;
  uint8_t secondTypeMask;
  uint64_t secondRegisterMask;

  c->arch->plan(Compare, size, &firstTypeMask, &firstRegisterMask,
                size, &secondTypeMask, &secondRegisterMask,
                &thunk);

  assert(c, not thunk); // todo

  new (c->zone->allocate(sizeof(CompareEvent)))
    CompareEvent
    (c, size, first, second,
     read(c, size, firstTypeMask, firstRegisterMask, AnyFrameIndex),
     read(c, size, secondTypeMask, secondRegisterMask, AnyFrameIndex));
}

void
move(Context* c, Stack* stack, Local* locals, unsigned size, Value* value,
     Site* src, Site* dst)
{
  if (dst->type(c) == MemoryOperand
      and src->type(c) == MemoryOperand)
  {
    Site* tmp = freeRegisterSite(c);
    addSite(c, stack, locals, size, value, tmp);
    apply(c, Move, size, src, size, tmp);
    src = tmp;
  }

  addSite(c, stack, locals, size, value, dst);
  apply(c, Move, size, src, size, dst);
}

void
preserve(Context* c, Stack* stack, Local* locals, unsigned size, Value* v,
         Site* s, Read* read)
{
  assert(c, v->sites == s);
  Site* r = targetOrNull(c, v, read);
  if (r == 0 or r == s) r = freeRegisterSite(c);
  move(c, stack, locals, size, v, s, r);
}

void
maybePreserve(Context* c, Stack* stack, Local* locals, unsigned size,
              Value* v, Site* s)
{
  if (valid(v->reads->next(c)) and v->sites->next == 0) {
    preserve(c, stack, locals, size, v, s, v->reads->next(c));
  }
}

class CombineEvent: public Event {
 public:
  CombineEvent(Context* c, TernaryOperation type,
               unsigned firstSize, Value* first,
               unsigned secondSize, Value* second,
               unsigned resultSize, Value* result,
               Read* firstRead,
               Read* secondRead,
               Read* resultRead):
    Event(c), type(type), firstSize(firstSize), first(first),
    secondSize(secondSize), second(second), resultSize(resultSize),
    result(result), resultRead(resultRead)
  {
    addRead(c, this, first, firstRead);
    addRead(c, this, second, secondRead);
  }

  virtual const char* name() {
    return "CombineEvent";
  }

  virtual void compile(Context* c) {
    Site* target;
    if (c->arch->condensedAddressing()) {
      maybePreserve(c, stackBefore, localsBefore, secondSize, second,
                    second->source);
      target = second->source;
    } else {
      target = resultRead->allocateSite(c);
      addSite(c, stackBefore, localsBefore, resultSize, result, target);
    }

//     fprintf(stderr, "combine %p and %p into %p\n", first, second, result);
    apply(c, type, firstSize, first->source, secondSize, second->source,
          resultSize, target);

    nextRead(c, this, first);
    nextRead(c, this, second);

    if (c->arch->condensedAddressing()) {
      removeSite(c, second, second->source);
      if (result->reads) {
        addSite(c, 0, 0, resultSize, result, second->source);
      }
    }
  }

  TernaryOperation type;
  unsigned firstSize;
  Value* first;
  unsigned secondSize;
  Value* second;
  unsigned resultSize;
  Value* result;
  Read* resultRead;
};

Value*
value(Context* c, Site* site = 0, Site* target = 0)
{
  return new (c->zone->allocate(sizeof(Value))) Value(site, target);
}

Stack*
stack(Context* c, Value* value, unsigned size, unsigned index, Stack* next)
{
  return new (c->zone->allocate(sizeof(Stack)))
    Stack(index, size, value, next);
}

Stack*
stack(Context* c, Value* value, unsigned size, Stack* next)
{
  return stack
    (c, value, size, (next ? next->index + next->size : 0), next);
}

void
push(Context* c, unsigned size, Value* v)
{
  assert(c, ceiling(size, BytesPerWord));

  c->stack = stack(c, v, ceiling(size, BytesPerWord), c->stack);
}

Value*
pop(Context* c, unsigned size UNUSED)
{
  Stack* s = c->stack;
  assert(c, ceiling(size, BytesPerWord) == s->size);

  c->stack = s->next;
  return s->value;
}

void
appendCombine(Context* c, TernaryOperation type,
              unsigned firstSize, Value* first,
              unsigned secondSize, Value* second,
              unsigned resultSize, Value* result)
{
  bool thunk;
  uint8_t firstTypeMask;
  uint64_t firstRegisterMask;
  uint8_t secondTypeMask;
  uint64_t secondRegisterMask;
  uint8_t resultTypeMask;
  uint64_t resultRegisterMask;

  c->arch->plan(type, firstSize, &firstTypeMask, &firstRegisterMask,
                secondSize, &secondTypeMask, &secondRegisterMask,
                resultSize, &resultTypeMask, &resultRegisterMask,
                &thunk);

  if (thunk) {
    Stack* oldStack = c->stack;

    ::push(c, secondSize, second);
    ::push(c, firstSize, first);

    Stack* argumentStack = c->stack;
    c->stack = oldStack;

    appendCall
      (c, value(c, constantSite(c, c->client->getThunk(type, resultSize))),
       0, 0, result, resultSize, argumentStack, 2, 0);
  } else {
    Read* resultRead = read
      (c, resultSize, resultTypeMask, resultRegisterMask, AnyFrameIndex);
    Read* secondRead;
    if (c->arch->condensedAddressing()) {
      secondRead = resultRead;
    } else {
      secondRead = read
        (c, secondSize, secondTypeMask, secondRegisterMask, AnyFrameIndex);
    }

    new (c->zone->allocate(sizeof(CombineEvent)))
      CombineEvent
      (c, type,
       firstSize, first,
       secondSize, second,
       resultSize, result,
       read(c, firstSize, firstTypeMask, firstRegisterMask, AnyFrameIndex),
       secondRead,
       resultRead);
  }
}

class TranslateEvent: public Event {
 public:
  TranslateEvent(Context* c, BinaryOperation type, unsigned size, Value* value,
                 Value* result, Read* read):
    Event(c), type(type), size(size), value(value), result(result)
  {
    addRead(c, this, value, read);
  }

  virtual const char* name() {
    return "TranslateEvent";
  }

  virtual void compile(Context* c) {
    maybePreserve(c, stackBefore, localsBefore, size, value, value->source);

    Site* target = targetOrRegister(c, result);
    apply(c, type, size, value->source, size, target);
    
    nextRead(c, this, value);

    removeSite(c, value, value->source);
    if (live(result)) {
      addSite(c, 0, 0, size, result, value->source);
    }
  }

  BinaryOperation type;
  unsigned size;
  Value* value;
  Value* result;
};

void
appendTranslate(Context* c, BinaryOperation type, unsigned size, Value* value,
                Value* result)
{
  bool thunk;
  uint8_t firstTypeMask;
  uint64_t firstRegisterMask;
  uint8_t resultTypeMask;
  uint64_t resultRegisterMask;

  c->arch->plan(type, size, &firstTypeMask, &firstRegisterMask,
                size, &resultTypeMask, &resultRegisterMask,
                &thunk);

  assert(c, not thunk); // todo

  // todo: respect resultTypeMask and resultRegisterMask

  new (c->zone->allocate(sizeof(TranslateEvent)))
    TranslateEvent
    (c, type, size, value, result,
     read(c, size, firstTypeMask, firstRegisterMask, AnyFrameIndex));
}

class MemoryEvent: public Event {
 public:
  MemoryEvent(Context* c, Value* base, int displacement, Value* index,
              unsigned scale, Value* result):
    Event(c), base(base), displacement(displacement), index(index),
    scale(scale), result(result)
  {
    addRead(c, this, base, anyRegisterRead(c, BytesPerWord));
    if (index) addRead(c, this, index, registerOrConstantRead(c, BytesPerWord));
  }

  virtual const char* name() {
    return "MemoryEvent";
  }

  virtual void compile(Context* c) {
    int indexRegister;
    int displacement = this->displacement;
    unsigned scale = this->scale;
    if (index) {
      ConstantSite* constant = findConstantSite(c, index);

      if (constant) {
        indexRegister = NoRegister;
        displacement += (constant->value.value->value() * scale);
        scale = 1;
      } else {
        assert(c, index->source->type(c) == RegisterOperand);
        indexRegister = static_cast<RegisterSite*>
          (index->source)->register_.low;
      }
    } else {
      indexRegister = NoRegister;
    }
    assert(c, base->source->type(c) == RegisterOperand);
    int baseRegister = static_cast<RegisterSite*>(base->source)->register_.low;

    nextRead(c, this, base);
    if (index) {
      if (BytesPerWord == 8 and indexRegister != NoRegister) {
        apply(c, Move, 4, index->source, 8, index->source);
      }

      nextRead(c, this, index);
    }

    result->target = memorySite
      (c, baseRegister, displacement, indexRegister, scale);
    addSite(c, 0, 0, 0, result, result->target);
  }

  Value* base;
  int displacement;
  Value* index;
  unsigned scale;
  Value* result;
};

void
appendMemory(Context* c, Value* base, int displacement, Value* index,
             unsigned scale, Value* result)
{
  new (c->zone->allocate(sizeof(MemoryEvent)))
    MemoryEvent(c, base, displacement, index, scale, result);
}

class BranchEvent: public Event {
 public:
  BranchEvent(Context* c, UnaryOperation type, Value* address):
    Event(c), type(type), address(address)
  {
    address->addPredecessor(c, this);

    addRead(c, this, address, read
            (c, BytesPerWord, ~0, ~static_cast<uint64_t>(0), AnyFrameIndex));
  }

  virtual const char* name() {
    return "BranchEvent";
  }

  virtual void compile(Context* c) {
    bool jump;
    UnaryOperation type = this->type;
    if (type != Jump) {
      switch (c->constantCompare) {
      case CompareLess:
        switch (type) {
        case JumpIfLess:
        case JumpIfLessOrEqual:
        case JumpIfNotEqual:
          jump = true;
          type = Jump;
          break;

        default:
          jump = false;
        }
        break;

      case CompareGreater:
        switch (type) {
        case JumpIfGreater:
        case JumpIfGreaterOrEqual:
        case JumpIfNotEqual:
          jump = true;
          type = Jump;
          break;

        default:
          jump = false;
        }
        break;

      case CompareEqual:
        switch (type) {
        case JumpIfEqual:
        case JumpIfLessOrEqual:
        case JumpIfGreaterOrEqual:
          jump = true;
          type = Jump;
          break;

        default:
          jump = false;
        }
        break;

      case CompareNone:
        jump = true;
        break;

      default: abort(c);
      }
    } else {
      jump = true;
    }

    if (jump) {
      apply(c, type, BytesPerWord, address->source);
    }

    nextRead(c, this, address);
  }

  UnaryOperation type;
  Value* address;
};

void
appendBranch(Context* c, UnaryOperation type, Value* address)
{
  new (c->zone->allocate(sizeof(BranchEvent)))
    BranchEvent(c, type, address);
}

class BoundsCheckEvent: public Event {
 public:
  BoundsCheckEvent(Context* c, Value* object, unsigned lengthOffset,
                   Value* index, intptr_t handler):
    Event(c), object(object), lengthOffset(lengthOffset), index(index),
    handler(handler)
  {
    addRead(c, this, object, anyRegisterRead(c, BytesPerWord));
    addRead(c, this, index, registerOrConstantRead(c, BytesPerWord));
  }

  virtual const char* name() {
    return "BoundsCheckEvent";
  }

  virtual void compile(Context* c) {
    Assembler* a = c->assembler;

    ConstantSite* constant = findConstantSite(c, index);
    CodePromise* nextPromise = codePromise
      (c, static_cast<Promise*>(0));
    CodePromise* outOfBoundsPromise = 0;

    if (constant) {
      expect(c, constant->value.value->value() >= 0);      
    } else {
      outOfBoundsPromise = codePromise(c, static_cast<Promise*>(0));

      apply(c, Compare, 4, constantSite(c, resolved(c, 0)), 4, index->source);

      Assembler::Constant outOfBoundsConstant(outOfBoundsPromise);
      a->apply
        (JumpIfLess, BytesPerWord, ConstantOperand, &outOfBoundsConstant);
    }

    assert(c, object->source->type(c) == RegisterOperand);
    int base = static_cast<RegisterSite*>(object->source)->register_.low;

    Site* length = memorySite(c, base, lengthOffset);
    length->acquire(c, 0, 0, 0, 0);

    apply(c, Compare, 4, index->source, 4, length);

    length->release(c);

    Assembler::Constant nextConstant(nextPromise);
    a->apply(JumpIfGreater, BytesPerWord, ConstantOperand, &nextConstant);

    if (constant == 0) {
      outOfBoundsPromise->offset = a->offset();
    }

    Assembler::Constant handlerConstant(resolved(c, handler));
    a->apply(Call, BytesPerWord, ConstantOperand, &handlerConstant);

    nextPromise->offset = a->offset();

    nextRead(c, this, object);
    nextRead(c, this, index);
  }

  Value* object;
  unsigned lengthOffset;
  Value* index;
  intptr_t handler;
};

void
appendBoundsCheck(Context* c, Value* object, unsigned lengthOffset,
                  Value* index, intptr_t handler)
{
  new (c->zone->allocate(sizeof(BoundsCheckEvent))) BoundsCheckEvent
    (c, object, lengthOffset, index, handler);
}

class FrameSiteEvent: public Event {
 public:
  FrameSiteEvent(Context* c, Value* value, unsigned size, int index):
    Event(c), value(value), size(size), index(index)
  { }

  virtual const char* name() {
    return "FrameSiteEvent";
  }

  virtual void compile(Context* c) {
    addSite(c, stackBefore, localsBefore, size, value, frameSite(c, index));
  }

  Value* value;
  unsigned size;
  int index;
};

void
appendFrameSite(Context* c, Value* value, unsigned size, int index)
{
  new (c->zone->allocate(sizeof(FrameSiteEvent))) FrameSiteEvent
    (c, value, size, index);
}

unsigned
frameFootprint(Context* c, Stack* s)
{
  return c->localFootprint + (s ? (s->index + s->size) : 0);
}

class DummyEvent: public Event {
 public:
  DummyEvent(Context* c):
    Event(c)
  { }

  virtual const char* name() {
    return "DummyEvent";
  }

  virtual void compile(Context*) { }
};

void
appendDummy(Context* c)
{
  Stack* stack = c->stack;
  Local* locals = c->locals;
  LogicalInstruction* i = c->logicalCode[c->logicalIp];

  c->stack = i->stack;
  c->locals = i->locals;

  new (c->zone->allocate(sizeof(DummyEvent))) DummyEvent(c);

  c->stack = stack;
  c->locals = locals;  
}

Site*
readSource(Context* c, Stack* stack, Local* locals, Read* r)
{
  if (r->value->sites == 0) {
    return 0;
  }

  Site* site = r->pickSite(c, r->value);

  if (site) {
    return site;
  } else {
    Site* target = r->allocateSite(c);
    unsigned copyCost;
    site = pick(c, r->value->sites, target, &copyCost);
    assert(c, copyCost);
    move(c, stack, locals, r->size, r->value, site, target);
    return target;    
  }
}

Site*
pickJunctionSite(Context* c, Value* v, Read* r, unsigned index)
{
  if (c->availableRegisterCount > 1) {
    Site* s = r->pickSite(c, v);
    if (s
        and ((1 << s->type(c))
             & ((1 << MemoryOperand)
                | (1 << RegisterOperand))))
    {
      return s;
    }

    s = r->allocateSite(c);
    if (s) return s;

    return freeRegisterSite(c);
  } else {
    return frameSite(c, index);
  }
}

unsigned
resolveJunctionSite(Context* c, Event* e, Value* v, unsigned index,
                    Site** frozenSites, unsigned frozenSiteIndex)
{
  assert(c, index < frameFootprint(c, e->stackAfter));

  if (live(v)) {
    assert(c, v->sites);
    
    Read* r = v->reads;
    Site* original = e->junctionSites[index];

    if (original == 0) {
      e->junctionSites[index] = pickJunctionSite(c, v, r, index);
    }

    Site* target = e->junctionSites[index];
    unsigned copyCost;
    Site* site = pick(c, v->sites, target, &copyCost);
    if (copyCost) {
      move(c, e->stackAfter, e->localsAfter, r->size, v, site, target);
    } else {
      target = site;
    }

    target->makeSpecific(c);

    char buffer[256]; target->toString(c, buffer, 256);
//     fprintf(stderr, "resolve junction site %d %s %p\n", index, buffer, target);

    if (original == 0) {
      frozenSites[frozenSiteIndex++] = target;
      target->freeze(c);
    }
  }

  return frozenSiteIndex;
}

void
propagateJunctionSites(Context* c, Event* e, Site** sites)
{
  for (Cell* pc = e->predecessors; pc; pc = pc->next) {
    Event* p = static_cast<Event*>(pc->value);
    if (p->junctionSites == 0) {
      p->junctionSites = sites;
      for (Cell* sc = p->successors; sc; sc = sc->next) {
        Event* s = static_cast<Event*>(sc->value);
        propagateJunctionSites(c, s, sites);
      }
    }
  }
}

void
populateSiteTables(Context* c, Event* e)
{
  unsigned frameFootprint = ::frameFootprint(c, e->stackAfter);

  { Site* frozenSites[frameFootprint];
    unsigned frozenSiteIndex = 0;

    if (e->junctionSites) {
      if (e->stackAfter) {
        unsigned i = e->stackAfter->index + c->localFootprint;
        for (Stack* stack = e->stackAfter; stack; stack = stack->next) {
          if (e->junctionSites[i]) {
            frozenSiteIndex = resolveJunctionSite
              (c, e, stack->value, i, frozenSites, frozenSiteIndex);
          
            i -= stack->size;
          }
        }
      }

      for (int i = c->localFootprint - 1; i >= 0; --i) {
        if (e->localsAfter[i].value and e->junctionSites[i]) {
          frozenSiteIndex = resolveJunctionSite
            (c, e, e->localsAfter[i].value, i, frozenSites, frozenSiteIndex);
        }
      }
    } else {
      for (Cell* sc = e->successors; sc; sc = sc->next) {
        Event* s = static_cast<Event*>(sc->value);
        if (s->predecessors->next) {
          unsigned size = sizeof(Site*) * frameFootprint;
          Site** junctionSites = static_cast<Site**>
            (c->zone->allocate(size));
          memset(junctionSites, 0, size);

          propagateJunctionSites(c, s, junctionSites);
          break;
        }
      }
    }

    if (e->junctionSites) {
      if (e->stackAfter) {
        unsigned i = e->stackAfter->index + c->localFootprint;
        for (Stack* stack = e->stackAfter; stack; stack = stack->next) {
          if (e->junctionSites[i] == 0) {
            frozenSiteIndex = resolveJunctionSite
              (c, e, stack->value, i, frozenSites, frozenSiteIndex);
          
            i -= stack->size;
          }
        }
      }

      for (int i = c->localFootprint - 1; i >= 0; --i) {
        if (e->localsAfter[i].value and e->junctionSites[i] == 0) {
          frozenSiteIndex = resolveJunctionSite
            (c, e, e->localsAfter[i].value, i, frozenSites, frozenSiteIndex);
        }
      }
    }

    while (frozenSiteIndex) {
      frozenSites[--frozenSiteIndex]->thaw(c);
    }
  }

  if (e->successors->next) {
    unsigned size = sizeof(Site*) * frameFootprint;
    Site** savedSites = static_cast<Site**>(c->zone->allocate(size));
    memset(savedSites, 0, size);

    for (unsigned i = 0; i < c->localFootprint; ++i) {
      Value* v = e->localsAfter[i].value;
      if (v) {
        savedSites[i] = v->sites;

//         fprintf(stderr, "save %p for %p at %d\n", savedSites[i], v, i);
      }
    }

    if (e->stackAfter) {
      unsigned i = e->stackAfter->index + c->localFootprint;
      for (Stack* stack = e->stackAfter; stack; stack = stack->next) {
        savedSites[i] = stack->value->sites;
//         fprintf(stderr, "save %p for %p at %d\n",
//                 savedSites[i], stack->value, i);

        i -= stack->size;
      }
    }

    e->savedSites = savedSites;
  }
}

void
setSites(Context* c, Event* e, Site** sites)
{
  for (unsigned i = 0; i < c->localFootprint; ++i) {
    Value* v = e->localsBefore[i].value;
    if (v) {
      clearSites(c, v);
      if (live(v)) {
//         fprintf(stderr, "set sites %p for %p at %d\n", sites[i], v, i);

        addSite(c, 0, 0, v->reads->size, v, sites[i]);
      }
    }
  }

  if (e->stackBefore) {
    unsigned i = e->stackBefore->index + c->localFootprint;
    for (Stack* stack = e->stackBefore; stack; stack = stack->next) {
      Value* v = stack->value;
      clearSites(c, v);
      if (live(v)) {
//         fprintf(stderr, "set sites %p for %p at %d\n", sites[i], v, i);

        addSite(c, 0, 0, v->reads->size, v, sites[i]);
      }
      i -= stack->size;
    }
  }
}

void
populateSources(Context* c, Event* e)
{
  Site* frozenSites[e->readCount];
  unsigned frozenSiteIndex = 0;
  for (Read* r = e->reads; r; r = r->eventNext) {
    r->value->source = readSource(c, e->stackBefore, e->localsBefore, r);

    if (r->value->source) {
      assert(c, frozenSiteIndex < e->readCount);
      frozenSites[frozenSiteIndex++] = r->value->source;
      r->value->source->freeze(c);
    }
  }

  while (frozenSiteIndex) {
    frozenSites[--frozenSiteIndex]->thaw(c);
  }
}

void
addStubRead(Context* c, Value* v, unsigned size, StubReadPair** reads)
{
  if (v) {
    StubRead* r;
    if (v->visited) {
      r = static_cast<StubRead*>(v->lastRead);
    } else {
      v->visited = true;

      r = stubRead(c, size);
      addRead(c, 0, v, r);
    }

    StubReadPair* p = (*reads)++;
    p->value = v;
    p->read = r;
  }
}

void
populateJunctionReads(Context* c, Event* e)
{
  StubReadPair* reads = static_cast<StubReadPair*>
    (c->zone->allocate(sizeof(StubReadPair) * frameFootprint(c, c->stack)));

  e->junctionReads = reads;  
     
  for (unsigned i = 0; i < c->localFootprint; ++i) {
    Local* local = c->locals + i;
    addStubRead(c, local->value, local->size, &reads);
  }
  
  for (Stack* s = c->stack; s; s = s->next) {
    addStubRead(c, s->value, s->size * BytesPerWord, &reads);
  }
  
  for (StubReadPair* r = e->junctionReads; r < reads; ++r) {
    r->value->visited = false;
  }
}

void
updateStubRead(Context*, StubReadPair* p, Read* r)
{
  if (p->read->read == 0) p->read->read = r;
}

void
updateJunctionReads(Context* c, Event* e)
{
  StubReadPair* reads = e->junctionReads;
     
  for (unsigned i = 0; i < c->localFootprint; ++i) {
    if (e->localsAfter[i].value) {
      updateStubRead(c, reads++, e->localsAfter[i].value->reads);
    }
  }
  
  for (Stack* s = e->stackAfter; s; s = s->next) {
    updateStubRead(c, reads++, s->value->reads);
  }
}

LogicalInstruction*
next(Context* c, LogicalInstruction* i)
{
  for (unsigned n = i->index + 1; n < c->logicalCodeLength; ++n) {
    i = c->logicalCode[n];
    if (i) return i;
  }
  return 0;
}

class Block {
 public:
  Block(Event* head):
    head(head), nextInstruction(0), assemblerBlock(0), start(0)
  { }

  Event* head;
  LogicalInstruction* nextInstruction;
  Assembler::Block* assemblerBlock;
  unsigned start;
};

Block*
block(Context* c, Event* head)
{
  return new (c->zone->allocate(sizeof(Block))) Block(head);
}

unsigned
compile(Context* c)
{
  if (c->logicalIp >= 0 and c->logicalCode[c->logicalIp]->lastEvent == 0) {
    appendDummy(c);
  }

  Assembler* a = c->assembler;

  c->pass = CompilePass;

  Block* firstBlock = block(c, c->firstEvent);
  Block* block = firstBlock;

  a->allocateFrame(c->alignedFrameSize);

  for (Event* e = c->firstEvent; e; e = e->next) {
    e->block = block;

    if (DebugCompile) {
      fprintf(stderr,
              "compile %s at %d with %d preds, %d succs, %d stack\n",
              e->name(), e->logicalInstruction->index,
              count(e->predecessors), count(e->successors),
              e->stackBefore ?
              e->stackBefore->index + e->stackBefore->size : 0);
    }

    if (e->logicalInstruction->machineOffset == 0) {
      e->logicalInstruction->machineOffset = a->offset();
    }

    MyState* state = e->state;
    if (state) {
      for (unsigned i = 0; i < state->readCount; ++i) {
        MultiReadPair* p = state->reads + i;
        p->value->reads = p->read->nextTarget();
      }
    }

    if (e->predecessors) {
      Event* predecessor = static_cast<Event*>(e->predecessors->value);
      if (e->predecessors->next) {
        for (Cell* cell = e->predecessors; cell->next; cell = cell->next) {
          updateJunctionReads(c, static_cast<Event*>(cell->value));
        }
        setSites(c, e, predecessor->junctionSites);
      } else if (predecessor->successors->next) {
        setSites(c, e, predecessor->savedSites);
      }
    }

    populateSources(c, e);

    e->compile(c);

    if (e->successors) {
      populateSiteTables(c, e);
    }

    e->compilePostsync(c);

    for (CodePromise* p = e->promises; p; p = p->next) {
      p->offset = a->offset();
    }
    
    LogicalInstruction* nextInstruction = next(c, e->logicalInstruction);
    if (e->next == 0
        or (e->next->logicalInstruction != e->logicalInstruction
            and (e->logicalInstruction->lastEvent == e
                 or e->next->logicalInstruction != nextInstruction)))
    {
      block->nextInstruction = nextInstruction;
      block->assemblerBlock = a->endBlock(e->next != 0);
      if (e->next) {
        block = ::block(c, e->next);
      }
    }
  }

  block = firstBlock;
  while (block->nextInstruction) {
    Block* next = block->nextInstruction->firstEvent->block;
    next->start = block->assemblerBlock->resolve
      (block->start, next->assemblerBlock);
    block = next;
  }

  return block->assemblerBlock->resolve(block->start, 0);
}

unsigned
count(Stack* s)
{
  unsigned c = 0;
  while (s) {
    ++ c;
    s = s->next;
  }
  return c;
}

void
allocateTargets(Context* c, MyState* state)
{
  for (unsigned i = 0; i < state->readCount; ++i) {
    MultiReadPair* p = state->reads + i;
    p->value->lastRead = p->read;
    p->read->allocateTarget(c);
  }
}

void
addMultiRead(Context* c, Value* v, unsigned size, MyState* state,
             unsigned* count)
{
  if (v and not v->visited) {
    v->visited = true;

    MultiRead* r = multiRead(c, size);
    addRead(c, 0, v, r);

    MultiReadPair* p = state->reads + ((*count)++);
    p->value = v;
    p->read = r;
  }
}

MyState*
saveState(Context* c)
{
  MyState* state = new
    (c->zone->allocate
     (sizeof(MyState) + (sizeof(MultiReadPair) * frameFootprint(c, c->stack))))
    MyState(c->stack, c->locals, c->predecessor, c->logicalIp);

  if (c->predecessor) {
    c->state = state;

    unsigned count = 0;

    for (unsigned i = 0; i < c->localFootprint; ++i) {
      if (c->locals[i].value) {
        Local* local = c->locals + i;
        addMultiRead(c, local->value, local->size, state, &count);
      }
    }
  
    for (Stack* s = c->stack; s; s = s->next) {
      addMultiRead(c, s->value, s->size * BytesPerWord, state, &count);
    }

    for (unsigned i = 0; i < count; ++i) {
      state->reads[i].value->visited = false;
    }

    state->readCount = count;

    allocateTargets(c, state);
  }

  return state;
}

void
restoreState(Context* c, MyState* s)
{
  if (c->logicalIp >= 0 and c->logicalCode[c->logicalIp]->lastEvent == 0) {
    appendDummy(c);
  }

  c->stack = s->stack;
  c->locals = s->locals;
  c->predecessor = s->predecessor;
  c->logicalIp = s->logicalIp;

  if (c->predecessor) {
    c->state = s;
    allocateTargets(c, s);
  }
}

class Client: public Assembler::Client {
 public:
  Client(Context* c): c(c) { }

  virtual int acquireTemporary(uint32_t mask) {
    int r = pickRegister(c, mask)->number;
    save(r);
    increment(c, r);
    return r;
  }

  virtual void releaseTemporary(int r) {
    decrement(c, c->registers[r]);
    restore(r);
  }

  virtual void save(int r) {
    // todo
    expect(c, c->registers[r]->refCount == 0);
    expect(c, c->registers[r]->value == 0);
  }

  virtual void restore(int) {
    // todo
  }

  Context* c;
};

class MyCompiler: public Compiler {
 public:
  MyCompiler(System* s, Assembler* assembler, Zone* zone,
             Compiler::Client* compilerClient):
    c(s, assembler, zone, compilerClient), client(&c)
  {
    assembler->setClient(&client);
  }

  virtual State* saveState() {
    return ::saveState(&c);
  }

  virtual void restoreState(State* state) {
    ::restoreState(&c, static_cast<MyState*>(state));
  }

  virtual void init(unsigned logicalCodeLength, unsigned parameterFootprint,
                    unsigned localFootprint, unsigned alignedFrameSize)
  {
    c.logicalCodeLength = logicalCodeLength;
    c.parameterFootprint = parameterFootprint;
    c.localFootprint = localFootprint;
    c.alignedFrameSize = alignedFrameSize;

    unsigned frameResourceSize = sizeof(FrameResource)
      * (alignedFrameSize + parameterFootprint);

    c.frameResources = static_cast<FrameResource*>
      (c.zone->allocate(frameResourceSize));

    memset(c.frameResources, 0, frameResourceSize);

    c.logicalCode = static_cast<LogicalInstruction**>
      (c.zone->allocate(sizeof(LogicalInstruction*) * logicalCodeLength));
    memset(c.logicalCode, 0, sizeof(LogicalInstruction*) * logicalCodeLength);

    c.locals = static_cast<Local*>
      (c.zone->allocate(sizeof(Local) * localFootprint));

    memset(c.locals, 0, sizeof(Local) * localFootprint);
  }

  virtual void visitLogicalIp(unsigned logicalIp) {
    assert(&c, logicalIp < c.logicalCodeLength);

    Event* e = c.logicalCode[logicalIp]->firstEvent;

    Event* p = c.predecessor;
    if (p) {
      p->stackAfter = c.stack;
      p->localsAfter = c.locals;

      p->successors = cons(&c, e, p->successors);
      populateJunctionReads(&c, p);
      e->predecessors = cons(&c, p, e->predecessors);
    }
  }

  virtual void startLogicalIp(unsigned logicalIp) {
    assert(&c, logicalIp < c.logicalCodeLength);
    assert(&c, c.logicalCode[logicalIp] == 0);

    if (DebugAppend) {
      fprintf(stderr, " -- ip: %d\n", logicalIp);
    }

    if (c.logicalIp >= 0 and c.logicalCode[c.logicalIp]->lastEvent == 0) {
      appendDummy(&c);
    }

    c.logicalCode[logicalIp] = new 
      (c.zone->allocate(sizeof(LogicalInstruction)))
      LogicalInstruction(logicalIp, c.stack, c.locals);

    c.logicalIp = logicalIp;
  }

  virtual Promise* machineIp(unsigned logicalIp) {
    return new (c.zone->allocate(sizeof(IpPromise))) IpPromise(&c, logicalIp);
  }

  virtual Promise* poolAppend(intptr_t value) {
    return poolAppendPromise(resolved(&c, value));
  }

  virtual Promise* poolAppendPromise(Promise* value) {
    Promise* p = new (c.zone->allocate(sizeof(PoolPromise)))
      PoolPromise(&c, c.constantCount);

    ConstantPoolNode* constant
      = new (c.zone->allocate(sizeof(ConstantPoolNode)))
      ConstantPoolNode(value);

    if (c.firstConstant) {
      c.lastConstant->next = constant;
    } else {
      c.firstConstant = constant;
    }
    c.lastConstant = constant;
    ++ c.constantCount;

    return p;
  }

  virtual Operand* constant(int64_t value) {
    return promiseConstant(resolved(&c, value));
  }

  virtual Operand* promiseConstant(Promise* value) {
    return ::value(&c, ::constantSite(&c, value));
  }

  virtual Operand* address(Promise* address) {
    return value(&c, ::addressSite(&c, address));
  }

  virtual Operand* memory(Operand* base,
                          int displacement = 0,
                          Operand* index = 0,
                          unsigned scale = 1)
  {
    Value* result = value(&c);

    appendMemory(&c, static_cast<Value*>(base), displacement,
                 static_cast<Value*>(index), scale, result);

    return result;
  }

  virtual Operand* stack() {
    Site* s = registerSite(&c, c.arch->stack());
    return value(&c, s, s);
  }

  virtual Operand* thread() {
    Site* s = registerSite(&c, c.arch->thread());
    return value(&c, s, s);
  }

  virtual Operand* stackTop() {
    Site* s = frameSite(&c, c.stack->index);
    return value(&c, s, s);
  }

  Promise* machineIp() {
    return codePromise(&c, c.logicalCode[c.logicalIp]->lastEvent);
  }

  virtual void push(unsigned size) {
    assert(&c, ceiling(size, BytesPerWord));

    c.stack = ::stack(&c, value(&c), ceiling(size, BytesPerWord), c.stack);
  }

  virtual void push(unsigned size, Operand* value) {
    ::push(&c, size, static_cast<Value*>(value));
  }

  virtual Operand* pop(unsigned size) {
    return ::pop(&c, size);
  }

  virtual void pushed() {
    Value* v = value(&c);
    appendFrameSite
      (&c, v, BytesPerWord,
       (c.stack ? c.stack->index + c.stack->size : c.localFootprint));

    c.stack = ::stack(&c, v, 1, c.stack);
  }

  virtual void popped() {
    c.stack = c.stack->next;
  }

  virtual StackElement* top() {
    return c.stack;
  }

  virtual unsigned size(StackElement* e) {
    return static_cast<Stack*>(e)->size;
  }

  virtual unsigned padding(StackElement* e) {
    return static_cast<Stack*>(e)->padding;
  }

  virtual Operand* peek(unsigned size UNUSED, unsigned index) {
    Stack* s = c.stack;
    for (unsigned i = index; i > 0;) {
      i -= s->size;
      s = s->next;
    }
    assert(&c, s->size == ceiling(size, BytesPerWord));
    return s->value;
  }

  virtual Operand* call(Operand* address,
                        unsigned flags,
                        TraceHandler* traceHandler,
                        unsigned resultSize,
                        unsigned argumentCount,
                        ...)
  {
    va_list a; va_start(a, argumentCount);

    unsigned footprint = 0;
    unsigned size = BytesPerWord;
    Value* arguments[argumentCount];
    unsigned argumentSizes[argumentCount];
    int index = 0;
    for (unsigned i = 0; i < argumentCount; ++i) {
      Value* o = va_arg(a, Value*);
      if (o) {
        arguments[index] = o;
        argumentSizes[index] = size;
        size = BytesPerWord;
        ++ index;
      } else {
        size = 8;
      }
      ++ footprint;
    }

    va_end(a);

    Stack* oldStack = c.stack;
    Stack* bottomArgument = 0;

    for (int i = index - 1; i >= 0; --i) {
      ::push(&c, argumentSizes[i], arguments[i]);
      if (i == index - 1) {
        bottomArgument = c.stack;
      }
    }
    Stack* argumentStack = c.stack;
    c.stack = oldStack;

    Value* result = value(&c);
    appendCall(&c, static_cast<Value*>(address), flags, traceHandler, result,
               resultSize, argumentStack, index, 0);

    return result;
  }

  virtual Operand* stackCall(Operand* address,
                             unsigned flags,
                             TraceHandler* traceHandler,
                             unsigned resultSize,
                             unsigned argumentFootprint)
  {
    Value* result = value(&c);
    appendCall(&c, static_cast<Value*>(address), flags, traceHandler, result,
               resultSize, c.stack, 0, argumentFootprint);
    return result;
  }

  virtual void return_(unsigned size, Operand* value) {
    appendReturn(&c, size, static_cast<Value*>(value));
  }

  virtual void initLocal(unsigned size, unsigned index) {
    assert(&c, index < c.localFootprint);

    Value* v = value(&c);
//     fprintf(stderr, "init local %p of size %d at %d\n", v, size, index);
    appendFrameSite(&c, v, size, index);

    Local* local = c.locals + index;
    local->value = v;
    local->size = size;
  }

  virtual void initLocalsFromLogicalIp(unsigned logicalIp) {
    assert(&c, logicalIp < c.logicalCodeLength);

    unsigned footprint = sizeof(Local) * c.localFootprint;
    Local* newLocals = static_cast<Local*>(c.zone->allocate(footprint));
    memset(newLocals, 0, footprint);
    c.locals = newLocals;

    Event* e = c.logicalCode[logicalIp]->firstEvent;
    for (unsigned i = 0; i < c.localFootprint; ++i) {
      Local* local = e->localsBefore + i;
      if (local->value) {
        initLocal(local->size, i);
      }
    }
  }

  virtual void storeLocal(unsigned size, Operand* src, unsigned index) {
    assert(&c, index < c.localFootprint);

    unsigned footprint = sizeof(Local) * c.localFootprint;
    Local* newLocals = static_cast<Local*>(c.zone->allocate(footprint));
    memcpy(newLocals, c.locals, footprint);
    c.locals = newLocals;

//     fprintf(stderr, "store local %p of size %d at %d\n", src, size, index);

    Local* local = c.locals + index;
    local->value = static_cast<Value*>(src);
    local->size = size;
  }

  virtual Operand* loadLocal(unsigned size UNUSED, unsigned index) {
    assert(&c, index < c.localFootprint);
    assert(&c, c.locals[index].value);
    assert(&c, pad(c.locals[index].size) == pad(size));

//     fprintf(stderr, "load local %p of size %d at %d\n",
//             c.locals[index].value, size, index);

    return c.locals[index].value;
  }

  virtual void checkBounds(Operand* object, unsigned lengthOffset,
                           Operand* index, intptr_t handler)
  {
    appendBoundsCheck(&c, static_cast<Value*>(object),
                      lengthOffset, static_cast<Value*>(index), handler);
  }

  virtual void store(unsigned size, Operand* src, Operand* dst) {
    appendMove(&c, Move, size, static_cast<Value*>(src),
               size, static_cast<Value*>(dst));
  }

  virtual Operand* load(unsigned size, Operand* src) {
    Value* dst = value(&c);
    appendMove(&c, Move, size, static_cast<Value*>(src), size, dst);
    return dst;
  }

  virtual Operand* loadz(unsigned size, Operand* src) {
    Value* dst = value(&c);
    appendMove(&c, MoveZ, size, static_cast<Value*>(src), size, dst);
    return dst;
  }

  virtual Operand* load4To8(Operand* src) {
    Value* dst = value(&c);
    appendMove(&c, Move, 4, static_cast<Value*>(src), 8, dst);
    return dst;
  }

  virtual Operand* lcmp(Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, LongCompare, 8, static_cast<Value*>(a),
                  8, static_cast<Value*>(b), 8, result);
    return result;
  }

  virtual void cmp(unsigned size, Operand* a, Operand* b) {
    appendCompare(&c, size, static_cast<Value*>(a),
                  static_cast<Value*>(b));
  }

  virtual void jl(Operand* address) {
    appendBranch(&c, JumpIfLess, static_cast<Value*>(address));
  }

  virtual void jg(Operand* address) {
    appendBranch(&c, JumpIfGreater, static_cast<Value*>(address));
  }

  virtual void jle(Operand* address) {
    appendBranch(&c, JumpIfLessOrEqual, static_cast<Value*>(address));
  }

  virtual void jge(Operand* address) {
    appendBranch(&c, JumpIfGreaterOrEqual, static_cast<Value*>(address));
  }

  virtual void je(Operand* address) {
    appendBranch(&c, JumpIfEqual, static_cast<Value*>(address));
  }

  virtual void jne(Operand* address) {
    appendBranch(&c, JumpIfNotEqual, static_cast<Value*>(address));
  }

  virtual void jmp(Operand* address) {
    appendBranch(&c, Jump, static_cast<Value*>(address));
  }

  virtual Operand* add(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Add, size, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* sub(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Subtract, size, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* mul(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Multiply, size, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* div(unsigned size, Operand* a, Operand* b)  {
    Value* result = value(&c);
    appendCombine(&c, Divide, size, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* rem(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Remainder, size, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* shl(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, ShiftLeft, BytesPerWord, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* shr(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, ShiftRight, BytesPerWord, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* ushr(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, UnsignedShiftRight, BytesPerWord, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* and_(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, And, size, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* or_(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Or, size, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* xor_(unsigned size, Operand* a, Operand* b) {
    Value* result = value(&c);
    appendCombine(&c, Xor, size, static_cast<Value*>(a),
                  size, static_cast<Value*>(b), size, result);
    return result;
  }

  virtual Operand* neg(unsigned size, Operand* a) {
    Value* result = value(&c);
    appendTranslate(&c, Negate, size, static_cast<Value*>(a), result);
    return result;
  }

  virtual unsigned compile() {
    return c.machineCodeSize = ::compile(&c);
  }

  virtual unsigned poolSize() {
    return c.constantCount * BytesPerWord;
  }

  virtual void writeTo(uint8_t* dst) {
    c.machineCode = dst;
    c.assembler->writeTo(dst);

    int i = 0;
    for (ConstantPoolNode* n = c.firstConstant; n; n = n->next) {
      *reinterpret_cast<intptr_t*>(dst + pad(c.machineCodeSize) + i)
        = n->promise->value();
      i += BytesPerWord;
    }
  }

  virtual void dispose() {
    // ignore
  }

  Context c;
  ::Client client;
};

} // namespace

namespace vm {

Compiler*
makeCompiler(System* system, Assembler* assembler, Zone* zone,
             Compiler::Client* client)
{
  return new (zone->allocate(sizeof(MyCompiler)))
    MyCompiler(system, assembler, zone, client);
}

} // namespace vm
