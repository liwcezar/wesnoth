/*
   Copyright (C) 2003 - 2016 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
   */

/**
 * @file
 * Wesnoth addon server.
 * Expects a "server.cfg" config file in the current directory
 * and saves addons under data/.
 */

#include "campaign_server/campaign_server.hpp"

#include "filesystem.hpp"
#include "log.hpp"
#include "network_worker.hpp"
#include "serialization/binary_or_text.hpp"
#include "serialization/parser.hpp"
#include "serialization/string_utils.hpp"
#include "serialization/unicode.hpp"
#include "game_config.hpp"
#include "addon/validation.hpp"
#include "campaign_server/addon_utils.hpp"
#include "campaign_server/blacklist.hpp"
#include "campaign_server/control.hpp"
#include "version.hpp"
#include "util.hpp"
#include "hash.hpp"

#include <csignal>
#include <ctime>

#include <boost/iostreams/filter/gzip.hpp>
#include <boost/exception/get_error_info.hpp>
#include <boost/random.hpp>
#include <boost/generator_iterator.hpp>

// the fork execute is unix specific only tested on Linux quite sure it won't
// work on Windows not sure which other platforms have a problem with it.
#if !(defined(_WIN32))
#include <errno.h>
#endif

static lg::log_domain log_campaignd("campaignd");
#define DBG_CS LOG_STREAM(debug, log_campaignd)
#define LOG_CS LOG_STREAM(info,  log_campaignd)
#define WRN_CS LOG_STREAM(warn,  log_campaignd)
#define ERR_CS LOG_STREAM(err,   log_campaignd)

//compatibility code for MS compilers
#ifndef SIGHUP
#define SIGHUP 20
#endif
/** @todo FIXME: should define SIGINT here too, but to what? */

namespace {

/**
 * Whether to reload the server configuration as soon as possible
 * (e.g. after SIGHUP).
 */
sig_atomic_t need_reload = 0;

void flag_sighup(int signal)
{
	assert(signal == SIGHUP);
	LOG_CS << "SIGHUP caught, scheduling config reload.\n";
	need_reload = 1;
}

void exit_sigint(int signal)
{
	assert(signal == SIGINT);
	LOG_CS << "SIGINT caught, exiting without cleanup immediately.\n";
	exit(0);
}

void exit_sigterm(int signal)
{
	assert(signal == SIGTERM);
	LOG_CS << "SIGTERM caught, exiting without cleanup immediately.\n";
	exit(128 + SIGTERM);
}

time_t monotonic_clock()
{
#if defined(_POSIX_MONOTONIC_CLOCK) && !defined(_WIN32)
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
#else
	#warning monotonic_clock() is not truly monotonic!
	return time(nullptr);
#endif
}

/* Secure password storage functions */
bool authenticate(config& campaign, const config::attribute_value& passphrase)
{
	return util::create_hash(passphrase, campaign["passsalt"]) == campaign["passhash"];
}

std::string generate_salt(size_t len)
{
	static const std::string itoa64 = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	boost::mt19937 mt(time(0));
	std::string salt = std::string(len, '0');
	boost::uniform_int<> from_str(0, itoa64.length() - 1);
	boost::variate_generator< boost::mt19937, boost::uniform_int<> > get_char(mt, from_str);

	for(size_t i = 0; i < len; i++) {
		salt[i] = itoa64[get_char()];
	}

	return salt;
}

void set_passphrase(config& campaign, std::string passphrase)
{
	std::string salt = generate_salt(16);
	campaign["passsalt"] = salt;
	campaign["passhash"] = util::create_hash(passphrase, salt);
}

} // end anonymous namespace

namespace campaignd {

server::server(const std::string& cfg_file, size_t min_threads, size_t max_threads)
	: cfg_()
	, cfg_file_(cfg_file)
	, read_only_(false)
	, compress_level_(0)
	, input_()
	, hooks_()
	, handlers_()
	, feedback_url_format_()
	, blacklist_()
	, blacklist_file_()
	, port_(load_config())
	, net_manager_(min_threads, max_threads)
	, server_manager_(port_)
{
#ifndef _MSC_VER
	signal(SIGHUP, flag_sighup);
#endif
	signal(SIGINT, exit_sigint);
	signal(SIGTERM, exit_sigterm);

	LOG_CS << "Port: " << port_ << "  Worker threads min/max: " << min_threads
		   << '/' << max_threads << '\n';

	// Ensure all campaigns to use secure hash passphrase storage
	if(!read_only_) {
		for(config& campaign : campaigns().child_range("campaign")) {
			// Campaign already has a hashed password
			if (campaign["passphrase"].empty()) {
				continue;
			}

			LOG_CS << "Campaign '" << campaign["title"] << "' uses unhashed passphrase. Fixing.\n";
			set_passphrase(campaign, campaign["passphrase"]);
			campaign["passphrase"] = "";
		}
		write_config();
	}

	register_handlers();
}

server::~server()
{
	write_config();
}

int server::load_config()
{
	LOG_CS << "Reading configuration from " << cfg_file_ << "...\n";

	filesystem::scoped_istream in = filesystem::istream_file(cfg_file_);
	read(cfg_, *in);

	read_only_ = cfg_["read_only"].to_bool(false);

	if(read_only_) {
		LOG_CS << "READ-ONLY MODE ACTIVE\n";
	}

	const bool use_system_sendfile = cfg_["network_use_system_sendfile"].to_bool();
	network_worker_pool::set_use_system_sendfile(use_system_sendfile);

	// Seems like compression level above 6 is a waste of CPU cycles.
	compress_level_ = cfg_["compress_level"].to_int(6);

	const config& svinfo_cfg = server_info();
	if(svinfo_cfg) {
		feedback_url_format_ = svinfo_cfg["feedback_url_format"].str();
	}

	blacklist_file_ = cfg_["blacklist_file"].str();
	load_blacklist();

	// Load any configured hooks.
	hooks_.insert(std::make_pair(std::string("hook_post_upload"), cfg_["hook_post_upload"]));
	hooks_.insert(std::make_pair(std::string("hook_post_erase"), cfg_["hook_post_erase"]));

	// Open the control socket if enabled.
	if(!cfg_["control_socket"].empty()) {
		const std::string& path = cfg_["control_socket"].str();

		if(!input_.get() || input_->path() != path) {
			input_.reset(new input_stream(cfg_["control_socket"]));
		}
	}

	// Ensure the campaigns list WML exists even if empty, other functions
	// depend on its existence.
	cfg_.child_or_add("campaigns");

	// Certain config values are saved to WML again so that a given server
	// instance's parameters remain constant even if the code defaults change
	// at some later point.
	cfg_["network_use_system_sendfile"] = use_system_sendfile;
	cfg_["compress_level"] = compress_level_;

	// But not the listening port number.
	return cfg_["port"].to_int(default_campaignd_port);
}

void server::load_blacklist()
{
	// We *always* want to clear the blacklist first, especially if we are
	// reloading the configuration and the blacklist is no longer enabled.
	blacklist_.clear();

	if(blacklist_file_.empty()) {
		return;
	}

	try {
		filesystem::scoped_istream in = filesystem::istream_file(blacklist_file_);
		config blcfg;

		read(blcfg, *in);

		blacklist_.read(blcfg);
		LOG_CS << "using blacklist from " << blacklist_file_ << '\n';
	} catch(const config::error&) {
		ERR_CS << "failed to read blacklist from " << blacklist_file_ << ", blacklist disabled\n";
	}
}

void server::write_config()
{
	DBG_CS << "writing configuration and add-ons list to disk...\n";
	filesystem::scoped_ostream out = filesystem::ostream_file(cfg_file_);
	write(*out, cfg_);
	DBG_CS << "... done\n";
}

void server::fire(const std::string& hook, const std::string& addon)
{
	const std::map<std::string, std::string>::const_iterator itor = hooks_.find(hook);
	if(itor == hooks_.end()) {
		return;
	}

	const std::string& script = itor->second;
	if(script.empty()) {
		return;
	}

#if defined(_WIN32)
	(void)addon;
	ERR_CS << "Tried to execute a script on an unsupported platform\n";
	return;
#else
	pid_t childpid;

	if((childpid = fork()) == -1) {
		ERR_CS << "fork failed while updating campaign " << addon << '\n';
		return;
	}

	if(childpid == 0) {
		// We are the child process. Execute the script. We run as a
		// separate thread sharing stdout/stderr, which will make the
		// log look ugly.
		execlp(script.c_str(), script.c_str(), addon.c_str(), static_cast<char *>(nullptr));

		// exec() and family never return; if they do, we have a problem
		std::cerr << "ERROR: exec failed with errno " << errno << " for addon " << addon
		          << '\n';
		exit(errno);

	} else {
		return;
	}
#endif
}

void server::send_message(const std::string& msg, network::connection sock)
{
	config cfg;
	cfg.add_child("message")["message"] = msg;
	network::send_data(cfg, sock);
}

void server::send_error(const std::string& msg, network::connection sock)
{
	config cfg;
	cfg.add_child("error")["message"] = msg;
	ERR_CS << "[" << network::ip_address(sock) << "]: " << msg << '\n';
	network::send_data(cfg, sock);
}

void server::run()
{
	network::connection sock = 0;

	time_t last_ts = monotonic_clock();

	for(;;)
	{
		if(need_reload) {
			load_config(); // TODO: handle port number config changes

			need_reload = 0;
			last_ts = 0;

			LOG_CS << "Reloaded configuration\n";
		}

		try {
			bool force_flush = false;
			std::string admin_cmd;

			if(input_ && input_->read_line(admin_cmd)) {
				control_line ctl = admin_cmd;

				if(ctl == "shut_down") {
					LOG_CS << "Shut down requested by admin, shutting down...\n";
					break;
				} else if(ctl == "readonly") {
					if(ctl.args_count()) {
						cfg_["read_only"] = read_only_ = utils::string_bool(ctl[1], true);
					}

					LOG_CS << "Read only mode: " << (read_only_ ? "enabled" : "disabled") << '\n';
				} else if(ctl == "flush") {
					force_flush = true;
					LOG_CS << "Flushing config to disk...\n";
				} else if(ctl == "reload") {
					if(ctl.args_count()) {
						if(ctl[1] == "blacklist") {
							LOG_CS << "Reloading blacklist...\n";
							load_blacklist();
						} else {
							ERR_CS << "Unrecognized admin reload argument: " << ctl[1] << '\n';
						}
					} else {
						LOG_CS << "Reloading all configuration...\n";
						need_reload = 1;
						// Avoid flush timer ellapsing
						continue;
					}
				} else if(ctl == "setpass") {
					if(ctl.args_count() != 2) {
						ERR_CS << "Incorrect number of arguments for 'setpass'\n";
					} else {
						const std::string& addon_id = ctl[1];
						const std::string& newpass = ctl[2];
						config& campaign = get_campaign(addon_id);

						if(!campaign) {
							ERR_CS << "Add-on '" << addon_id << "' not found, cannot set passphrase\n";
						} else if(newpass.empty()) {
							// Shouldn't happen!
							ERR_CS << "Add-on passphrases may not be empty!\n";
						} else {
							set_passphrase(campaign, newpass);
							write_config();
							LOG_CS << "New passphrase set for '" << addon_id << "'\n";
						}
					}
				} else {
					ERR_CS << "Unrecognized admin command: " << ctl.full() << '\n';
				}
			}

			const time_t cur_ts = monotonic_clock();
			// Write config to disk every ten minutes.
			if(force_flush || labs(cur_ts - last_ts) >= 10*60) {
				write_config();
				last_ts = cur_ts;
			}

			network::process_send_queue();

			sock = network::accept_connection();
			if(sock) {
				LOG_CS << "received connection from " << network::ip_address(sock) << "\n";
			}

			config data;

			while((sock = network::receive_data(data, 0)) != network::null_connection)
			{
				config::all_children_iterator i = data.ordered_begin();

				if(i != data.ordered_end()) {
					// We only handle the first child.
					const config::any_child& c = *i;

					request_handlers_table::const_iterator j
							= handlers_.find(c.key);

					if(j != handlers_.end()) {
						// Call the handler.
						j->second(this, request(c.key, c.cfg, sock));
					} else {
						send_error("Unrecognized [" + c.key + "] request.",
								   sock);
					}
				}
			}
		} catch(network::error& e) {
			if(!e.socket) {
				ERR_CS << "fatal network error: " << e.message << "\n";
				throw;
			} else {
				LOG_CS << "client disconnect: " << e.message << " " << network::ip_address(e.socket) << "\n";
				e.disconnect();
			}
		} catch(const config::error& e) {
			network::connection err_sock = 0;
			network::connection const * err_connection = boost::get_error_info<network::connection_info>(e);

			if(err_connection != nullptr) {
				err_sock = *err_connection;
			}

			if(err_sock == 0 && sock > 0) {
				err_sock = sock;
			}

			if(err_sock) {
				ERR_CS << "client disconnect due to exception: " << e.what() << " " << network::ip_address(err_sock) << "\n";
				network::disconnect(err_sock);
			} else {
				throw;
			}
		}

		SDL_Delay(20);
	}
}

void server::register_handler(const std::string& cmd, const request_handler& func)
{
	handlers_[cmd] = func;
}

#define REGISTER_CAMPAIGND_HANDLER(req_id) \
	register_handler(#req_id, &server::handle_##req_id)

void server::register_handlers()
{
	REGISTER_CAMPAIGND_HANDLER(request_campaign_list);
	REGISTER_CAMPAIGND_HANDLER(request_campaign);
	REGISTER_CAMPAIGND_HANDLER(request_terms);
	REGISTER_CAMPAIGND_HANDLER(upload);
	REGISTER_CAMPAIGND_HANDLER(delete);
	REGISTER_CAMPAIGND_HANDLER(change_passphrase);
}

void server::handle_request_campaign_list(const server::request& req)
{
	LOG_CS << "sending campaign list to " << req.addr << " using gzip";

	time_t epoch = time(nullptr);
	config campaign_list;

	campaign_list["timestamp"] = epoch;
	if(req.cfg["times_relative_to"] != "now") {
		epoch = 0;
	}

	bool before_flag = false;
	time_t before = epoch;
	try {
		before = before + lexical_cast<time_t>(req.cfg["before"]);
		before_flag = true;
	} catch(bad_lexical_cast) {}

	bool after_flag = false;
	time_t after = epoch;
	try {
		after = after + lexical_cast<time_t>(req.cfg["after"]);
		after_flag = true;
	} catch(bad_lexical_cast) {}

	const std::string& name = req.cfg["name"];
	const std::string& lang = req.cfg["language"];

	for(const config& i : campaigns().child_range("campaign"))
	{
		if(!name.empty() && name != i["name"]) {
			continue;
		}

		const std::string& tm = i["timestamp"];

		if(before_flag && (tm.empty() || lexical_cast_default<time_t>(tm, 0) >= before)) {
			continue;
		}
		if(after_flag && (tm.empty() || lexical_cast_default<time_t>(tm, 0) <= after)) {
			continue;
		}

		if(!lang.empty()) {
			bool found = false;

			for(const config& j : i.child_range("translation"))
			{
				if(j["language"] == lang) {
					found = true;
					break;
				}
			}

			if(!found) {
				continue;
			}
		}

		campaign_list.add_child("campaign", i);
	}

	for(config& j : campaign_list.child_range("campaign"))
	{
		j["passphrase"] = "";
		j["passhash"] = "";
		j["passsalt"] = "";
		j["upload_ip"] = "";
		j["email"] = "";
		j["feedback_url"] = "";

		// Build a feedback_url string attribute from the
		// internal [feedback] data.
		const config& url_params = j.child_or_empty("feedback");
		if(!url_params.empty() && !feedback_url_format_.empty()) {
			j["feedback_url"] = format_addon_feedback_url(feedback_url_format_, url_params);
		}

		// Clients don't need to see the original data, so discard it.
		j.clear_children("feedback");
	}

	config response;
	response.add_child("campaigns", campaign_list);

	std::cerr << " size: " << (network::send_data(response, req.sock)/1024) << "KiB\n";
}

void server::handle_request_campaign(const server::request& req)
{
	LOG_CS << "sending campaign '" << req.cfg["name"] << "' to " << req.addr << " using gzip";

	config& campaign = get_campaign(req.cfg["name"]);

	if(!campaign) {
		send_error("Add-on '" + req.cfg["name"].str() + "' not found.", req.sock);
	} else {
		const int size = filesystem::file_size(campaign["filename"]);

		if(size < 0) {
			std::cerr << " size: <unknown> KiB\n";
			ERR_CS << "File size unknown, aborting send.\n";
			send_error("Add-on '" + req.cfg["name"].str() + "' could not be read by the server.", req.sock);
			return;
		}

		std::cerr << " size: " << size/1024 << "KiB\n";
		network::send_file(campaign["filename"], req.sock);
		// Clients doing upgrades or some other specific thing shouldn't bump
		// the downloads count. Default to true for compatibility with old
		// clients that won't tell us what they are trying to do.
		if(req.cfg["increase_downloads"].to_bool(true)) {
			const int downloads = campaign["downloads"].to_int() + 1;
			campaign["downloads"] = downloads;
		}
	}
}

void server::handle_request_terms(const server::request& req)
{
	// This usually means the client wants to upload content, so tell it
	// to give up when we're in read-only mode.
	if(read_only_) {
		LOG_CS << "in read-only mode, request for upload terms denied\n";
		send_error("The server is currently in read-only mode, add-on uploads are disabled.", req.sock);
		return;
	}

	LOG_CS << "sending terms " << req.addr << "\n";
	send_message("All add-ons uploaded to this server must be licensed under the terms of the GNU General Public License (GPL). By uploading content to this server, you certify that you have the right to place the content under the conditions of the GPL, and choose to do so.", req.sock);
	LOG_CS << " Done\n";
}

void server::handle_upload(const server::request& req)
{
	const config& upload = req.cfg;

	LOG_CS << "uploading campaign '" << upload["name"] << "' from " << req.addr << ".\n";
	config data = upload.child("data");

	const std::string& name = upload["name"];
	config *campaign = nullptr;

	bool passed_name_utf8_check = false;

	try {
		const std::string& lc_name = utf8::lowercase(name);
		passed_name_utf8_check = true;

		for(config& c : campaigns().child_range("campaign"))
		{
			if(utf8::lowercase(c["name"]) == lc_name) {
				campaign = &c;
				break;
			}
		}
	} catch(const utf8::invalid_utf8_exception&) {
		if(!passed_name_utf8_check) {
			LOG_CS << "Upload aborted - invalid_utf8_exception caught on handle_upload() check 1, "
			       << "the add-on pbl info contains invalid UTF-8\n";
			send_error("Add-on rejected: The add-on name contains an invalid UTF-8 sequence.", req.sock);
		} else {
			LOG_CS << "Upload aborted - invalid_utf8_exception caught on handle_upload() check 2, "
			       << "the internal add-ons list contains invalid UTF-8\n";
			send_error("Server error: The server add-ons list is damaged.", req.sock);
		}

		return;
	}

	if(read_only_) {
		LOG_CS << "Upload aborted - uploads not permitted in read-only mode.\n";
		send_error("Add-on rejected: The server is currently in read-only mode.", req.sock);
	} else if(!data) {
		LOG_CS << "Upload aborted - no add-on data.\n";
		send_error("Add-on rejected: No add-on data was supplied.", req.sock);
	} else if(!addon_name_legal(upload["name"])) {
		LOG_CS << "Upload aborted - invalid add-on name.\n";
		send_error("Add-on rejected: The name of the add-on is invalid.", req.sock);
	} else if(is_text_markup_char(upload["name"].str()[0])) {
		LOG_CS << "Upload aborted - add-on name starts with an illegal formatting character.\n";
		send_error("Add-on rejected: The name of the add-on starts with an illegal formatting character.", req.sock);
	} else if(upload["title"].empty()) {
		LOG_CS << "Upload aborted - no add-on title specified.\n";
		send_error("Add-on rejected: You did not specify the title of the add-on in the pbl file!", req.sock);
	} else if(is_text_markup_char(upload["title"].str()[0])) {
		LOG_CS << "Upload aborted - add-on title starts with an illegal formatting character.\n";
		send_error("Add-on rejected: The title of the add-on starts with an illegal formatting character.", req.sock);
	} else if(get_addon_type(upload["type"]) == ADDON_UNKNOWN) {
		LOG_CS << "Upload aborted - unknown add-on type specified.\n";
		send_error("Add-on rejected: You did not specify a known type for the add-on in the pbl file! (See PblWML: wiki.wesnoth.org/PblWML)", req.sock);
	} else if(upload["author"].empty()) {
		LOG_CS << "Upload aborted - no add-on author specified.\n";
		send_error("Add-on rejected: You did not specify the author(s) of the add-on in the pbl file!", req.sock);
	} else if(upload["version"].empty()) {
		LOG_CS << "Upload aborted - no add-on version specified.\n";
		send_error("Add-on rejected: You did not specify the version of the add-on in the pbl file!", req.sock);
	} else if(upload["description"].empty()) {
		LOG_CS << "Upload aborted - no add-on description specified.\n";
		send_error("Add-on rejected: You did not specify a description of the add-on in the pbl file!", req.sock);
	} else if(upload["email"].empty()) {
		LOG_CS << "Upload aborted - no add-on email specified.\n";
		send_error("Add-on rejected: You did not specify your email address in the pbl file!", req.sock);
	} else if(!check_names_legal(data)) {
		LOG_CS << "Upload aborted - invalid file names in add-on data.\n";
		send_error("Add-on rejected: The add-on contains an illegal file or directory name."
				   " File or directory names may not contain whitespace or any of the following characters: '/ \\ : ~'",
				   req.sock);
	} else if(campaign && !authenticate(*campaign, upload["passphrase"])) {
		LOG_CS << "Upload aborted - incorrect passphrase.\n";
		send_error("Add-on rejected: The add-on already exists, and your passphrase was incorrect.", req.sock);
	} else {
		const time_t upload_ts = time(nullptr);

		LOG_CS << "Upload is owner upload.\n";

		try {
			if(blacklist_.is_blacklisted(name,
										 upload["title"].str(),
										 upload["description"].str(),
										 upload["author"].str(),
										 req.addr,
										 upload["email"].str()))
			{
				LOG_CS << "Upload denied - blacklisted add-on information.\n";
				send_error("Add-on upload denied. Please contact the server administration for assistance.", req.sock);
				return;
			}
		} catch(const utf8::invalid_utf8_exception&) {
			LOG_CS << "Upload aborted - the add-on pbl info contains invalid UTF-8 and cannot be "
				   << "checked against the blacklist\n";
			send_error("Add-on rejected: The add-on publish information contains an invalid UTF-8 sequence.", req.sock);
			return;
		}

		const bool existing_upload = campaign != nullptr;

		std::string message = "Add-on accepted.";

		if(campaign == nullptr) {
			campaign = &campaigns().add_child("campaign");
			(*campaign)["original_timestamp"] = upload_ts;
		}

		(*campaign)["title"] = upload["title"];
		(*campaign)["name"] = upload["name"];
		(*campaign)["filename"] = "data/" + upload["name"].str();
		(*campaign)["author"] = upload["author"];
		(*campaign)["description"] = upload["description"];
		(*campaign)["version"] = upload["version"];
		(*campaign)["icon"] = upload["icon"];
		(*campaign)["translate"] = upload["translate"];
		(*campaign)["dependencies"] = upload["dependencies"];
		(*campaign)["upload_ip"] = req.addr;
		(*campaign)["type"] = upload["type"];
		(*campaign)["email"] = upload["email"];

		if(!existing_upload) {
			set_passphrase(*campaign, upload["passphrase"]);
		}

		if((*campaign)["downloads"].empty()) {
			(*campaign)["downloads"] = 0;
		}
		(*campaign)["timestamp"] = upload_ts;

		int uploads = (*campaign)["uploads"].to_int() + 1;
		(*campaign)["uploads"] = uploads;

		(*campaign).clear_children("feedback");
		if(const config& url_params = upload.child("feedback")) {
			(*campaign).add_child("feedback", url_params);
		}

		const std::string& filename = (*campaign)["filename"].str();
		data["title"] = (*campaign)["title"];
		data["name"] = "";
		data["campaign_name"] = (*campaign)["name"];
		data["author"] = (*campaign)["author"];
		data["description"] = (*campaign)["description"];
		data["version"] = (*campaign)["version"];
		data["timestamp"] = (*campaign)["timestamp"];
		data["original_timestamp"] = (*campaign)["original_timestamp"];
		data["icon"] = (*campaign)["icon"];
		data["type"] = (*campaign)["type"];
		(*campaign).clear_children("translation");
		find_translations(data, *campaign);

		add_license(data);

		{
			filesystem::scoped_ostream campaign_file = filesystem::ostream_file(filename);
			config_writer writer(*campaign_file, true, compress_level_);
			writer.write(data);
		}

		(*campaign)["size"] = filesystem::file_size(filename);

		write_config();

		send_message(message, req.sock);

		fire("hook_post_upload", upload["name"]);
	}
}

void server::handle_delete(const server::request& req)
{
	const config& erase = req.cfg;

	if(read_only_) {
		LOG_CS << "in read-only mode, request to delete '" << erase["name"] << "' from " << req.addr << " denied\n";
		send_error("Cannot delete add-on: The server is currently in read-only mode.", req.sock);
		return;
	}

	LOG_CS << "deleting campaign '" << erase["name"] << "' requested from " << req.addr << "\n";

	config& campaign = get_campaign(erase["name"]);

	if(!campaign) {
		send_error("The add-on does not exist.", req.sock);
		return;
	}

	if(!authenticate(campaign, erase["passphrase"])
	   && (campaigns()["master_password"].empty()
	   || campaigns()["master_password"] != erase["passphrase"]))
	{
		send_error("The passphrase is incorrect.", req.sock);
		return;
	}

	// Erase the campaign.
	filesystem::write_file(campaign["filename"], std::string());
	if(remove(campaign["filename"].str().c_str()) != 0) {
		ERR_CS << "failed to delete archive for campaign '" << erase["name"]
			   << "' (" << campaign["filename"] << "): " << strerror(errno)
			   << '\n';
	}

	config::child_itors itors = campaigns().child_range("campaign");
	for(size_t index = 0; itors.first != itors.second; ++index, ++itors.first)
	{
		if(&campaign == &*itors.first) {
			campaigns().remove_child("campaign", index);
			break;
		}
	}

	write_config();

	send_message("Add-on deleted.", req.sock);

	fire("hook_post_erase", erase["name"]);

}

void server::handle_change_passphrase(const server::request& req)
{
	const config& cpass = req.cfg;

	if(read_only_) {
		LOG_CS << "in read-only mode, request to change passphrase denied\n";
		send_error("Cannot change passphrase: The server is currently in read-only mode.", req.sock);
		return;
	}

	config& campaign = get_campaign(cpass["name"]);

	if(!campaign) {
		send_error("No add-on with that name exists.", req.sock);
	} else if(!authenticate(campaign, cpass["passphrase"])) {
		send_error("Your old passphrase was incorrect.", req.sock);
	} else if(cpass["new_passphrase"].empty()) {
		send_error("No new passphrase was supplied.", req.sock);
	} else {
		set_passphrase(campaign, cpass["new_passphrase"]);
		write_config();
		send_message("Passphrase changed.", req.sock);
	}
}

} // end namespace campaignd

int main(int argc, char**argv)
{
	game_config::path = filesystem::get_cwd();

	lg::set_log_domain_severity("campaignd", lg::info());
	lg::timestamps(true);

	try {
		std::cerr << "Wesnoth campaignd v" << game_config::revision << " starting...\n";

		const std::string& cfg_path = filesystem::normalize_path("server.cfg");

		if(argc >= 2 && atoi(argv[1])){
			campaignd::server(cfg_path, atoi(argv[1])).run();
		} else {
			campaignd::server(cfg_path).run();
		}
	} catch(config::error& /*e*/) {
		std::cerr << "Could not parse config file\n";
		return 1;
	} catch(filesystem::io_exception& /*e*/) {
		std::cerr << "File I/O error\n";
		return 2;
	} catch(network::error& e) {
		std::cerr << "Aborted with network error: " << e.message << '\n';
		return 3;
	} catch(std::bad_function_call& /*e*/) {
		std::cerr << "Bad request handler function call\n";
		return 4;
	}

	return 0;
}
