// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <test/harness.h>

namespace noticeboard_basic
{
  struct Alive : public VCown<Alive>
  {
  public:
    int n = 10;
  };

  struct Ping : public VAction<Ping>
  {
    void f() {}
  };

  struct C : public V<C>
  {
  public:
    int x = 0;
    C* next = nullptr;
    Alive* alive = nullptr;

    C(int x_) : x(x_) {}

    void trace(ObjectStack* st) const
    {
      if (next != nullptr)
        st->push(next);
      if (alive != nullptr)
        st->push(alive);
    }
  };

  struct DB : public VCown<DB>
  {
  public:
    Noticeboard<Object*> box;
    int n_max = 40;
    int n = 0;

    DB(Object* c) : box{c}
    {
#ifdef USE_SYSTEMATIC_TESTING
      register_noticeboard(&box);
#endif
    }

    void trace(ObjectStack* fields) const
    {
      box.trace(fields);
    }
  };

  enum Phase
  {
    INIT,
    WAITFORGC,
    PEEK,
    WAITFORCOLLECTION,
    USEALIVE,
    EXIT,
  };

  struct Peeker : public VCown<Peeker>
  {
  public:
    DB* db;
    Noticeboard<Object*>* box;
    Alive* alive = nullptr;
    Phase state = INIT;
    int wait_for_collection = 600;
    int wait_for_gc_n = 100;

    Peeker(DB* db_, Noticeboard<Object*>* box_) : db(db_), box(box_) {}

    void trace(ObjectStack* fields) const
    {
      if (alive != nullptr)
      {
        fields->push(alive);
      }
      assert(db);
      fields->push(db);
    }
  };

  struct UpdateDB : public VAction<UpdateDB>
  {
    DB* db;
    UpdateDB(DB* db) : db(db) {}

    void f()
    {
      auto* alloc = ThreadAlloc::get();
      if (db->n == 30)
      {
        C* new_c = new (alloc) C(1);
        Freeze::apply(alloc, new_c);
        db->box.update(alloc, new_c);
      }

      if (db->n != db->n_max)
      {
        db->n++;
        Cown::schedule<UpdateDB>(db, db);
      }
    }
  };

  struct ToPeek : public VAction<ToPeek>
  {
    Peeker* peeker;
    ToPeek(Peeker* peeker) : peeker(peeker) {}

    void f()
    {
      auto* alloc = ThreadAlloc::get();
      (void)alloc;
      switch (peeker->state)
      {
        case INIT: {
          Cown::schedule<UpdateDB>(peeker->db, peeker->db);
          Scheduler::want_ld();
          peeker->state = WAITFORGC;
          Cown::schedule<ToPeek>(peeker, peeker);
          return;
        }
        case WAITFORGC: {
          if (peeker->wait_for_gc_n == 0)
          {
            peeker->state = PEEK;
          }
          else
          {
            peeker->wait_for_gc_n--;
          }
          Cown::schedule<ToPeek>(peeker, peeker);
          return;
        }
        case PEEK: {
          auto o = (C*)peeker->box->peek(alloc);
          if (o->alive == nullptr)
          {
            peeker->state = EXIT;
          }
          else
          {
            assert(o->alive);
            Cown::acquire(o->alive);
            peeker->alive = o->alive;
            peeker->state = WAITFORCOLLECTION;
          }
          // o goes out of scope
          Immutable::release(alloc, o);
          Cown::schedule<ToPeek>(peeker, peeker);
          return;
        }
        case WAITFORCOLLECTION: {
          if (peeker->wait_for_collection == 0)
          {
            peeker->state = USEALIVE;
          }
          else
          {
            peeker->wait_for_collection--;
          }
          Cown::schedule<ToPeek>(peeker, peeker);
          return;
        }
        case USEALIVE: {
          Cown::schedule<Ping>(peeker->alive);
          peeker->state = EXIT;
          Cown::schedule<ToPeek>(peeker, peeker);
          return;
        }
        case EXIT: {
          return;
        }
      }
      assert(false);
    }
  };

  void run_test()
  {
    Alloc* alloc = ThreadAlloc::get();

    Alive* alive = new Alive;

    C* c = new (alloc) C(0);
    c->next = new (alloc) C(10);

    RegionTrace::insert(alloc, c, alive);
    c->alive = alive;

    Freeze::apply(alloc, c);

    DB* db = new DB(c);
    Peeker* peeker = new Peeker(db, &db->box);

    Cown::schedule<ToPeek>(peeker, peeker);
    Cown::schedule<Ping>(alive);

    Cown::release(alloc, alive);
    Cown::release(alloc, peeker);
    // Ownership of db was transferred to Peeker, no need to release it.
  }
}
