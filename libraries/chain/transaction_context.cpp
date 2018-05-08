#include <enumivo/chain/apply_context.hpp>
#include <enumivo/chain/transaction_context.hpp>
#include <enumivo/chain/exceptions.hpp>
#include <enumivo/chain/resource_limits.hpp>
#include <enumivo/chain/generated_transaction_object.hpp>
#include <enumivo/chain/transaction_object.hpp>
#include <enumivo/chain/global_property_object.hpp>

namespace eosio { namespace chain {

   transaction_context::transaction_context( controller& c,
                                             const signed_transaction& t,
                                             const transaction_id_type& trx_id )
   :control(c)
   ,trx(t)
   ,id(trx_id)
   ,undo_session(c.db().start_undo_session(true))
   ,trace(std::make_shared<transaction_trace>())
   ,net_usage(trace->net_usage)
   ,cpu_usage(trace->cpu_usage)
   {
      trace->id = id;
      executed.reserve( trx.total_actions() );
      FC_ASSERT( trx.transaction_extensions.size() == 0, "we don't support any extensions yet" );
   }

   void transaction_context::init(uint64_t initial_net_usage, uint64_t initial_cpu_usage )
   {
      FC_ASSERT( !is_initialized, "cannot initialize twice" );

      // Record accounts to be billed for network and CPU usage
      uint64_t determine_payers_cpu_cost = 0;
      for( const auto& act : trx.actions ) {
         for( const auto& auth : act.authorization ) {
            bill_to_accounts.insert( auth.actor );
            determine_payers_cpu_cost += config::determine_payers_cpu_overhead_per_authorization;
         }
      }
      validate_ram_usage.reserve( bill_to_accounts.size() );

      // Calculate network and CPU usage limits and initial usage:

      // Start with limits set in dynamic configuration
      const auto& cfg = control.get_global_properties().configuration;
      max_net = cfg.max_transaction_net_usage;
      max_cpu = cfg.max_transaction_cpu_usage;

      // Potentially lower limits to what is optionally set in the transaction header
      uint64_t trx_specified_net_usage_limit = static_cast<uint64_t>(trx.max_net_usage_words.value)*8;
      if( trx_specified_net_usage_limit > 0 )
         max_net = std::min( max_net, trx_specified_net_usage_limit );
      uint64_t trx_specified_cpu_usage_limit = static_cast<uint64_t>(trx.max_kcpu_usage.value)*1024;
      if( trx_specified_cpu_usage_limit > 0 )
         max_cpu = std::min( max_cpu, trx_specified_cpu_usage_limit );

      eager_net_limit = max_net;
      eager_cpu_limit = max_cpu;
      //idump((eager_cpu_limit)(max_cpu));

      // Update usage values of accounts to reflect new time
      auto& rl = control.get_mutable_resource_limits_manager();
      rl.add_transaction_usage( bill_to_accounts, 0, 0, block_timestamp_type(control.pending_block_time()).slot );

      uint64_t block_net_limit = rl.get_block_net_limit();
      uint64_t block_cpu_limit = rl.get_block_cpu_limit();

      if( block_net_limit < eager_net_limit ) {
         eager_net_limit = block_net_limit;
         net_limit_due_to_block = true;
      }
      if( block_cpu_limit < eager_cpu_limit ) {
         eager_cpu_limit = block_cpu_limit;
         //idump((eager_cpu_limit)(max_cpu));
         cpu_limit_due_to_block = true;
      }

      // Initial billing for network usage
      if( initial_net_usage > 0 )
         add_net_usage( initial_net_usage );

      // Initial billing for CPU usage known to be soon consumed
      add_cpu_usage( initial_cpu_usage
                     + static_cast<uint64_t>(cfg.base_per_transaction_cpu_usage)
                     + determine_payers_cpu_cost
                     + bill_to_accounts.size() * config::resource_processing_cpu_overhead_per_billed_account );
      // Fails early if current CPU usage is already greater than the current limit (which may still go lower).


      eager_net_limit = max_net;
      eager_cpu_limit = max_cpu;
      //idump((eager_cpu_limit)(max_cpu));
      net_limit_due_to_block = false;
      cpu_limit_due_to_block = false;

      // Lower limits to what the billed accounts can afford to pay
      for( const auto& a : bill_to_accounts ) {
         auto net_limit = rl.get_account_net_limit(a);
         if( net_limit >= 0 )
            eager_net_limit = std::min( eager_net_limit, static_cast<uint64_t>(net_limit) ); // reduce max_net to the amount the account is able to pay

         auto cpu_limit = rl.get_account_cpu_limit(a);
         if( cpu_limit >= 0 ) {
            eager_cpu_limit = std::min( eager_cpu_limit, static_cast<uint64_t>(cpu_limit) ); // reduce max_cpu to the amount the account is able to pay
         }
      }

      initial_max_billable_cpu = eager_cpu_limit; // Possibly used for hard failure purposes

      eager_net_limit += cfg.net_usage_leeway;
      eager_net_limit = std::min(eager_net_limit, max_net);
      eager_cpu_limit += cfg.cpu_usage_leeway;
    //  idump((eager_cpu_limit)(max_cpu));
      eager_cpu_limit = std::min(eager_cpu_limit, max_cpu);
    //  idump((eager_cpu_limit)(max_cpu));

      if( block_net_limit < eager_net_limit ) {
         eager_net_limit = block_net_limit;
         net_limit_due_to_block = true;
      }
      if( block_cpu_limit < eager_cpu_limit ) {
         eager_cpu_limit = block_cpu_limit;
         cpu_limit_due_to_block = true;
      }

      // Round down network and CPU usage limits so that comparison to actual usage is more efficient
      eager_net_limit = (eager_net_limit/8)*8;       // Round down to nearest multiple of word size (8 bytes)
      eager_cpu_limit = (eager_cpu_limit/1024)*1024; // Round down to nearest multiple of 1024
      //idump((eager_cpu_limit));

      if( initial_net_usage > 0 )
         check_net_usage();  // Fail early if current net usage is already greater than the calculated limit
      check_cpu_usage(); // Fail early if current CPU usage is already greater than the calculated limit

      //idump((eager_cpu_limit));
      is_initialized = true;
   }

   void transaction_context::init_for_implicit_trx( fc::time_point d, uint64_t initial_net_usage, uint64_t initial_cpu_usage )
   {
      published = control.pending_block_time();
      deadline = d;
      init( initial_net_usage, initial_cpu_usage );
   }

   void transaction_context::init_for_input_trx( fc::time_point d,
                                                 uint64_t packed_trx_unprunable_size,
                                                 uint64_t packed_trx_prunable_size,
                                                 uint32_t num_signatures              )
   {
      const auto& cfg = control.get_global_properties().configuration;

      uint64_t discounted_size_for_pruned_data = packed_trx_prunable_size;
      if( cfg.context_free_discount_net_usage_den > 0
          && cfg.context_free_discount_net_usage_num < cfg.context_free_discount_net_usage_den )
      {
         discounted_size_for_pruned_data *= cfg.context_free_discount_net_usage_num;
         discounted_size_for_pruned_data =  ( discounted_size_for_pruned_data + cfg.context_free_discount_net_usage_den - 1)
                                                                                    / cfg.context_free_discount_net_usage_den; // rounds up
      }

      uint64_t initial_net_usage = static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                                    + packed_trx_unprunable_size + discounted_size_for_pruned_data;

      uint64_t initial_cpu_usage = num_signatures * cfg.per_signature_cpu_usage;

      if( trx.delay_sec.value > 0 ) {
          // If delayed, also charge ahead of time for the additional net usage needed to retire the delayed transaction
          // whether that be by successfully executing, soft failure, hard failure, or expiration.
         initial_net_usage += static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                               + static_cast<uint64_t>(config::transaction_id_net_usage);
      }

      published = control.pending_block_time();
      deadline = d;
      is_input = true;
      control.validate_expiration( trx );
      control.validate_tapos( trx );
      control.validate_referenced_accounts( trx );
      init( initial_net_usage, initial_cpu_usage );
      record_transaction( id, trx.expiration ); /// checks for dupes
   }

   void transaction_context::init_for_deferred_trx( fc::time_point d,
                                                    fc::time_point p )
   {
      published = p;
      deadline = d;
      trace->scheduled = true;
      apply_context_free = false;
      init( 0, 0 );
   }

   void transaction_context::exec() {
      FC_ASSERT( is_initialized, "must first initialize" );

      if( apply_context_free ) {
         for( const auto& act : trx.context_free_actions ) {
            trace->action_traces.emplace_back();
            dispatch_action( trace->action_traces.back(), act, true );
         }
      }

      if( delay == fc::microseconds() ) {
         for( const auto& act : trx.actions ) {
            trace->action_traces.emplace_back();
            dispatch_action( trace->action_traces.back(), act );
         }
      } else {
         schedule_transaction();
      }
   }

   void transaction_context::finalize() {
      FC_ASSERT( is_initialized, "must first initialize" );

      add_cpu_usage( validate_ram_usage.size() * config::ram_usage_validation_overhead_per_account );

      auto& rl = control.get_mutable_resource_limits_manager();
      for( auto a : validate_ram_usage ) {
         rl.verify_account_ram_usage( a );
      }

      eager_net_limit = max_net;
      eager_cpu_limit = max_cpu;

      net_limit_due_to_block = false;
      cpu_limit_due_to_block = false;

      // Lower limits to what the billed accounts can afford to pay
      for( const auto& a : bill_to_accounts ) {
         auto net_limit = rl.get_account_net_limit(a);
         if( net_limit >= 0 )
            eager_net_limit = std::min( eager_net_limit, static_cast<uint64_t>(net_limit) ); // reduce max_net to the amount the account is able to pay

         auto cpu_limit = rl.get_account_cpu_limit(a);
         if( cpu_limit >= 0 )
            eager_cpu_limit = std::min( eager_cpu_limit, static_cast<uint64_t>(cpu_limit) ); // reduce max_cpu to the amount the account is able to pay
      }

      net_usage = ((net_usage + 7)/8)*8; // Round up to nearest multiple of word size (8 bytes)
      cpu_usage = ((cpu_usage + 1023)/1024)*1024; // Round up to nearest multiple of 1024

      check_net_usage();
      check_cpu_usage();

      rl.add_transaction_usage( bill_to_accounts, cpu_usage, net_usage,
                                block_timestamp_type(control.pending_block_time()).slot ); // Should never fail
   }

   void transaction_context::squash() {
      undo_session.squash();
   }

   void transaction_context::add_cpu_usage_and_check_time( uint32_t u ) {
      add_cpu_usage( u );
      check_time();
   }

   uint64_t transaction_context::add_action_cpu_usage( uint64_t u, bool context_free ) {
      const auto& cfg = control.get_global_properties().configuration;

      uint64_t discounted_cpu_usage = u;
      if( context_free && cfg.context_free_discount_cpu_usage_den > 0
          && cfg.context_free_discount_cpu_usage_num < cfg.context_free_discount_cpu_usage_den )
      {
         discounted_cpu_usage *= cfg.context_free_discount_cpu_usage_num;
         discounted_cpu_usage =  ( discounted_cpu_usage + cfg.context_free_discount_cpu_usage_den - 1)
                                                               / cfg.context_free_discount_cpu_usage_den; // rounds up
      }

      add_cpu_usage( discounted_cpu_usage );
      return discounted_cpu_usage;
   }

   uint64_t transaction_context::get_action_cpu_usage_limit( bool context_free )const {
      check_cpu_usage();
      uint64_t diff = max_cpu - cpu_usage;
      if( !context_free ) return diff;

      const auto& cfg = control.get_global_properties().configuration;
      uint64_t n = cfg.context_free_discount_cpu_usage_num;
      uint64_t d = cfg.context_free_discount_cpu_usage_den;

      if( d == 0 || n >= d ) return diff;

      if( n == 0 ) return std::numeric_limits<uint64_t>::max();

      return (diff * d)/n;
   }

   void transaction_context::check_net_usage()const {
      if( BOOST_UNLIKELY(net_usage > eager_net_limit) ) {
         if( BOOST_UNLIKELY( net_limit_due_to_block ) ) {
            ENU_THROW( tx_soft_net_usage_exceeded,
                       "not enough space left in block: ${actual_net_usage} > ${net_usage_limit}",
                       ("actual_net_usage", net_usage)("net_usage_limit", max_net) );
         } else {
            ENU_THROW( tx_net_usage_exceeded,
                       "net usage of transaction is too high: ${actual_net_usage} > ${net_usage_limit}",
                       ("actual_net_usage", net_usage)("net_usage_limit", max_net) );
         }
      }
   }

   void transaction_context::check_cpu_usage()const {
      if( BOOST_UNLIKELY(cpu_usage > eager_cpu_limit) ) {
         if( BOOST_UNLIKELY( cpu_limit_due_to_block ) ) {
            ENU_THROW( tx_soft_cpu_usage_exceeded,
                       "not enough CPU usage allotment left in block: ${actual_cpu_usage} > ${cpu_usage_limit}",
                       ("actual_cpu_usage", cpu_usage)("cpu_usage_limit", max_cpu) );
         } else {
            ENU_THROW( tx_cpu_usage_exceeded,
                       "cpu usage of transaction is too high: ${actual_cpu_usage} > ${eager_cpu_limit}",
                       ("actual_cpu_usage", cpu_usage)("cpu_usage_limit", max_cpu)("eager_cpu_limit", eager_cpu_limit) );
         }
      }
   }

   void transaction_context::check_time()const {
      ENU_ASSERT( BOOST_LIKELY(fc::time_point::now() <= deadline), tx_deadline_exceeded, "deadline exceeded" );
   }

   void transaction_context::add_ram_usage( account_name account, int64_t ram_delta ) {
      auto& rl = control.get_mutable_resource_limits_manager();
      rl.add_pending_ram_usage( account, ram_delta );
      if( ram_delta > 0 ) {
         validate_ram_usage.insert( account );
      }
   }

   void transaction_context::dispatch_action( action_trace& trace, const action& a, account_name receiver, bool context_free, uint32_t recurse_depth ) {
      apply_context  acontext( control, *this, a, recurse_depth );
      acontext.context_free = context_free;
      acontext.receiver     = receiver;

      try {
         acontext.exec();
      } catch( const action_cpu_usage_exceeded& e ) {
         trace = move(acontext.trace);
         add_action_cpu_usage( acontext.cpu_usage, context_free ); // Will update cpu_usage to latest value and throw appropriate exception
         FC_ASSERT(false, "should not have reached here" );
      } catch( ... ) {
         trace = move(acontext.trace);
         throw;
      }

      trace = move(acontext.trace);
   }

   void transaction_context::schedule_transaction() {
      // Charge ahead of time for the additional net usage needed to retire the delayed transaction
      // whether that be by successfully executing, soft failure, hard failure, or expiration.
      if( trx.delay_sec.value == 0 ) { // Do not double bill. Only charge if we have not already charged for the delay.
         const auto& cfg = control.get_global_properties().configuration;
         add_net_usage( static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                         + static_cast<uint64_t>(config::transaction_id_net_usage) ); // Will exit early if net usage cannot be payed.
      }

      auto first_auth = trx.first_authorizor();

      uint32_t trx_size = 0;
      const auto& cgto = control.db().create<generated_transaction_object>( [&]( auto& gto ) {
        gto.trx_id      = id;
        gto.payer       = first_auth;
        gto.sender      = account_name(); /// delayed transactions have no sender
        gto.sender_id   = transaction_id_to_sender_id( gto.trx_id );
        gto.published   = control.pending_block_time();
        gto.delay_until = gto.published + delay;
        gto.expiration  = gto.delay_until + fc::seconds(control.get_global_properties().configuration.deferred_trx_expiration_window);
        trx_size = gto.set( trx );
      });

      add_ram_usage( cgto.payer, (config::billable_size_v<generated_transaction_object> + trx_size) );
   }

   void transaction_context::record_transaction( const transaction_id_type& id, fc::time_point_sec expire ) {
      try {
          control.db().create<transaction_object>([&](transaction_object& transaction) {
              transaction.trx_id = id;
              transaction.expiration = expire;
          });
      } catch ( ... ) {
          ENU_ASSERT( false, tx_duplicate,
                     "duplicate transaction ${id}", ("id", id ) );
      }
   } /// record_transaction


} } /// eosio::chain
