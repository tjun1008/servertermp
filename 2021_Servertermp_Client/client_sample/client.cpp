#define SFML_STATIC 1
#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <iostream>
#include <fstream>
#include <unordered_map>
//#include <windows.h>
#include <chrono>
#include <queue>
#include <thread>
#include "protocol.h"

using namespace std;
using namespace chrono;

thread* chat_thread;

#ifdef _DEBUG
#pragma comment (lib, "lib/sfml-graphics-s-d.lib")
#pragma comment (lib, "lib/sfml-window-s-d.lib")
#pragma comment (lib, "lib/sfml-system-s-d.lib")
#pragma comment (lib, "lib/sfml-network-s-d.lib")
#else
#pragma comment (lib, "lib/sfml-graphics-s.lib")
#pragma comment (lib, "lib/sfml-window-s.lib")
#pragma comment (lib, "lib/sfml-system-s.lib")
#pragma comment (lib, "lib/sfml-network-s.lib")
#endif
#pragma comment (lib, "opengl32.lib")
#pragma comment (lib, "winmm.lib")
#pragma comment (lib, "ws2_32.lib")

sf::TcpSocket socket;


constexpr auto SCREEN_WIDTH = 20;
constexpr auto SCREEN_HEIGHT = 20;

constexpr auto TILE_WIDTH = 65;
constexpr auto WINDOW_WIDTH = TILE_WIDTH * SCREEN_WIDTH + 10;   // size of window
constexpr auto WINDOW_HEIGHT = TILE_WIDTH * SCREEN_WIDTH + 10;
constexpr auto BUF_SIZE = MAX_BUFFER;

enum NPC_TYPE { PEACE, AGRO };
// Peace: : 때리기 전에는 가만히
//Agro : 근처에 11x11 영역에 접근하면 쫓아 오기

enum NPC_MOVE { FIX, ROAMING };

int g_left_x;
int g_top_y;
int g_myid;

bool chatting_func = false;
queue<string> chatqueue;

sf::RenderWindow* g_window;
sf::Font g_font;

class OBJECT {
private:
	bool m_showing;
	sf::Sprite m_sprite;
	sf::Text m_name;
	sf::Text m_level;
	sf::Text m_hp;
	sf::Text m_chat;
	chrono::system_clock::time_point m_mess_end_time;


public:
	int m_x, m_y;
	char name[MAX_ID_LEN];
	int level;
	int hp;
	int exp;

	char npc_Type;
	char npc_Move;

	OBJECT(sf::Texture& t, int x, int y, int x2, int y2) {
		m_showing = false;
		m_sprite.setTexture(t);
		m_sprite.setTextureRect(sf::IntRect(x, y, x2, y2));
		set_name("NONAME");
		m_mess_end_time = chrono::system_clock::now();
	}
	OBJECT() {
		m_showing = false;
		m_mess_end_time = chrono::system_clock::now();
	}
	void show()
	{
		m_showing = true;
	}
	void hide()
	{
		m_showing = false;
	}

	void a_move(int x, int y) {
		m_sprite.setPosition((float)x, (float)y);
	}

	void a_draw() {
		g_window->draw(m_sprite);
	}

	void move(int x, int y) {
		m_x = x;
		m_y = y;
	}
	void draw() {
		if (false == m_showing) return;
		float rx = (m_x - g_left_x) * 65.0f + 8;
		float ry = (m_y - g_top_y) * 65.0f + 8;
		m_sprite.setPosition(rx, ry);
		g_window->draw(m_sprite);
		if (chrono::system_clock::now() < m_mess_end_time) {
			m_chat.setPosition(rx - 10, ry - 20);
			g_window->draw(m_chat);

		}
		else {
			m_name.setPosition(rx - 10, ry - 20);
			g_window->draw(m_name);

			m_level.setPosition(rx - 10, ry - 40);
			g_window->draw(m_level);

			m_hp.setPosition(rx - 10, ry - 60);
			g_window->draw(m_hp);
		}
	}
	void set_name(const char* str) {
		m_name.setFont(g_font);
		m_name.setString(str);
		m_name.setFillColor(sf::Color(255, 255, 0));
		m_name.setStyle(sf::Text::Bold);
	}

	void set_level(const char* str) {
		m_level.setFont(g_font);
		m_level.setString(str);
		m_level.setFillColor(sf::Color(255, 255, 255));
		m_level.setStyle(sf::Text::Bold);
	}

	void set_hp(const char* str) {
		m_hp.setFont(g_font);
		m_hp.setString(str);
		m_hp.setFillColor(sf::Color(255, 0, 0));
		m_hp.setStyle(sf::Text::Bold);
	}

	void set_chat(char str[]) {
		m_chat.setFont(g_font);
		m_chat.setString(str);
		m_mess_end_time = system_clock::now() + 1s;
	}
};



OBJECT avatar;
unordered_map <int, OBJECT> players;
unordered_map <int, OBJECT> npcs;

OBJECT origin_tile;
OBJECT block_tile;
OBJECT npc_monster;

sf::Texture* board;
sf::Texture* block;
sf::Texture* pieces;
sf::Texture* monster;



bool g_Map[WORLD_WIDTH][WORLD_HEIGHT];



void client_initialize()
{
	board = new sf::Texture;
	block = new sf::Texture;
	pieces = new sf::Texture;
	monster = new sf::Texture;

	if (false == g_font.loadFromFile("cour.ttf")) {
		cout << "Font Loading Error!\n";
		while (true);
	}

	board->loadFromFile("wheat.bmp");
	block->loadFromFile("block.bmp");
	pieces->loadFromFile("chess2.png");
	monster->loadFromFile("monster.png");

	origin_tile = OBJECT{ *board, 0, 0, TILE_WIDTH, TILE_WIDTH };
	block_tile = OBJECT{ *block, 0, 0, TILE_WIDTH, TILE_WIDTH };
	avatar = OBJECT{ *pieces, 128, 0, 64, 64 };
	npc_monster = OBJECT{ *pieces, 0, 0, 64, 64 };
	//avatar.move(4, 4);
	avatar.hide();

	/*
	for (auto& pl : players) {
		pl = OBJECT{ *pieces, 64, 0, 64, 64 };
	}
	for (auto& pl : npcs) {
		pl = OBJECT{ *pieces, 0, 0, 64, 64 };
	}
	*/
}

void client_finish()
{
	delete board;
	delete block;
	delete pieces;
	delete monster;
}

void ProcessPacket(char* ptr)
{
	static bool first_time = true;
	switch (ptr[1])
	{
	case SC_LOGIN_OK:
	{
		sc_packet_login_ok* packet = reinterpret_cast<sc_packet_login_ok*>(ptr);
		g_myid = packet->id;
		avatar.m_x = packet->x;
		avatar.m_y = packet->y;
		g_left_x = packet->x - SCREEN_WIDTH / 2;
		g_top_y = packet->y - SCREEN_HEIGHT / 2;
		avatar.move(packet->x, packet->y);
		avatar.level = packet->LEVEL;
		avatar.hp = packet->HP;
		avatar.exp = packet->EXP;
		strcpy(avatar.name, packet->name);

		char buf[20];

		sprintf(buf, "Hp : %d", avatar.hp);
		avatar.set_hp(buf);

		sprintf(buf, "Level : %d", packet->LEVEL);
		avatar.set_level(buf);

		avatar.set_name(avatar.name);

		//리스폰

		avatar.show();
	}
	break;
	case SC_LOGIN_FAIL:
	{
		sc_packet_login_fail* packet = reinterpret_cast<sc_packet_login_fail*>(ptr);
		cout << "Login Fail " << endl;

		//this_thread::sleep_for(3s);
		exit(-1);
	}
	break;
	case SC_POSITION:
	{

		sc_packet_position* my_packet = reinterpret_cast<sc_packet_position*>(ptr);
		int other_id = my_packet->id;

		if (other_id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
			g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);

		}
		else if (other_id < NPC_ID_START) {
			players[other_id].move(my_packet->x, my_packet->y);
		}
		else {
			npcs[other_id].move(my_packet->x, my_packet->y);


		}
		break;
	}
	case SC_REMOVE_OBJECT:
	{
		sc_packet_remove_object* my_packet = reinterpret_cast<sc_packet_remove_object*>(ptr);
		int other_id = my_packet->id;
		if (other_id == g_myid) {
			avatar.hide();
		}
		else if (other_id < NPC_ID_START) {
			players[other_id].hide();
			players.erase(other_id);
		}
		else {
			npcs[other_id].hide();
			npcs.erase(other_id);
		}

	}
	break;
	case SC_ADD_OBJECT:
	{
		sc_packet_add_object* my_packet = reinterpret_cast<sc_packet_add_object*>(ptr);
		int id = my_packet->id;

		char buf[20];



		if (my_packet->obj_class == 0) {
			players[id] = OBJECT{ *pieces, 64, 0, 64, 64 };
			players[id].move(my_packet->x, my_packet->y);



			sprintf(buf, "Hp : %d", avatar.hp);
			players[id].set_hp(buf);

			sprintf(buf, "Level : %d", my_packet->LEVEL);
			players[id].set_level(buf);

			players[id].set_name(my_packet->name);

			players[id].show();
		}
		else {

			if (my_packet->monster_type == PEACE && my_packet->monster_move == FIX)
				npcs[id] = OBJECT{ *monster, 0, 0, 64, 64 };
			else if (my_packet->monster_type == PEACE && my_packet->monster_move == ROAMING)
				npcs[id] = OBJECT{ *monster, 64, 0, 64, 64 };
			else if (my_packet->monster_type == AGRO && my_packet->monster_move == FIX)
				npcs[id] = OBJECT{ *monster, 128, 0, 64, 64 };
			else if (my_packet->monster_type == AGRO && my_packet->monster_move == ROAMING)
				npcs[id] = OBJECT{ *monster, 192, 0, 64, 64 };

			npcs[id].npc_Type = my_packet->monster_type;
			npcs[id].npc_Move = my_packet->monster_move;

			npcs[id].move(my_packet->x, my_packet->y);

			sprintf(buf, "Hp : %d", my_packet->HP);
			npcs[id].set_hp(buf);

			sprintf(buf, "Level : %d", my_packet->LEVEL);
			npcs[id].set_level(buf);
			npcs[id].set_name(my_packet->name);

			npcs[id].show();
		}
	}
	break;

	case SC_CHAT:
	{
		sc_packet_chat* my_packet = reinterpret_cast<sc_packet_chat*>(ptr);
		int o_id = my_packet->id;

		string temp = "[Player: ";
		temp += to_string(o_id);
		temp += "] => ";
		temp += my_packet->message;
		chatqueue.push(temp);

		//큐가 5개 이상일 경우 POP
		if (chatqueue.size() > 5)
			chatqueue.pop();

	}
	break;
	case SC_STAT_CHANGE:
	{
		sc_packet_stat_change* my_packet = reinterpret_cast<sc_packet_stat_change*>(ptr);

		int id = my_packet->id;




		if (id == g_myid)
		{
			//cout << my_packet->HP << endl;
			avatar.exp = my_packet->EXP;
			avatar.hp = my_packet->HP;
			avatar.level = my_packet->LEVEL;

			char buf[20];
			sprintf(buf, "Hp : %d", my_packet->HP);
			avatar.set_hp(buf);

			sprintf(buf, "Level : %d", my_packet->LEVEL);
			avatar.set_level(buf);

			avatar.show();
		}
		else if (id < NPC_ID_START) {
			players[id].exp = my_packet->EXP;
			players[id].hp = my_packet->HP;
			players[id].level = my_packet->LEVEL;
			char buf[20];

			sprintf(buf, "Hp : %d", my_packet->HP);
			players[id].set_hp(buf);

			sprintf(buf, "Level : %d", my_packet->LEVEL);
			players[id].set_level(buf);

			players[id].show();
		}
		else
		{
			npcs[id].exp = my_packet->EXP;
			npcs[id].hp = my_packet->HP;
			npcs[id].level = my_packet->LEVEL;
			char buf[20];

			sprintf(buf, "Hp : %d", my_packet->HP);
			npcs[id].set_hp(buf);

			sprintf(buf, "Level : %d", my_packet->LEVEL);
			npcs[id].set_level(buf);

			npcs[id].show();
		}


	}
	break;
	default:
		printf("Unknown PACKET type [%d]\n", ptr[1]);
	}
}

void process_data(char* net_buf, size_t io_byte)
{
	char* ptr = net_buf;
	static size_t in_packet_size = 0;
	static size_t saved_packet_size = 0;
	static char packet_buffer[BUF_SIZE];

	while (0 != io_byte) {
		if (0 == in_packet_size) in_packet_size = ptr[0];
		if (io_byte + saved_packet_size >= in_packet_size) {
			memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);
			ProcessPacket(packet_buffer);
			ptr += in_packet_size - saved_packet_size;
			io_byte -= in_packet_size - saved_packet_size;
			in_packet_size = 0;
			saved_packet_size = 0;
		}
		else {
			memcpy(packet_buffer + saved_packet_size, ptr, io_byte);
			saved_packet_size += io_byte;
			io_byte = 0;
		}
	}
}

void send_chat_packet(const char* chat)
{
	chatting_func = false;

	//채팅 전송
	cs_packet_chat packet;
	packet.type = CS_CHAT;
	packet.size = sizeof(packet);
	strcpy(packet.message, chat);
	cout << packet.message << endl;
	size_t sent = 0;
	socket.send(&packet, sizeof(packet), sent);


}


void Chatting()
{
	chatting_func = true;

	char chat[MAX_STR_LEN];
	memset(&chat, 0, sizeof(chat));

	cout << "Chatting: ";
	int cnt = 0;


	scanf_s("%s", chat, MAX_STR_LEN);
	string temp(chat);

	if (temp.size() != 0)
	{
		send_chat_packet(chat);
	}




}




void showChat()
{
	queue<string> temp;


	for (int i = 0; i < 5; i++)
	{
		if (!chatqueue.empty()) {

			sf::Text Chat;
			Chat.setFont(g_font);
			char hp_buf[100];

			strcpy_s(hp_buf, chatqueue.front().c_str());
			Chat.setString(hp_buf);
			Chat.setPosition(10, 1000 + i * 50);
			Chat.setCharacterSize(40);
			if (i % 2 == 0)
				Chat.setFillColor(sf::Color::Magenta);
			else
				Chat.setFillColor(sf::Color::White);
			Chat.setStyle(sf::Text::Bold);
			g_window->draw(Chat);

			string cd;
			cd = chatqueue.front();
			chatqueue.pop();
			temp.push(cd);
		}
		else break;
	}
	chatqueue = temp;
}

void client_main()
{
	char net_buf[BUF_SIZE];
	size_t	received;

	auto recv_result = socket.receive(net_buf, BUF_SIZE, received);
	if (recv_result == sf::Socket::Error)
	{
		wcout << L"Recv 에러!";
		while (true);
	}
	if (recv_result != sf::Socket::NotReady)
		if (received > 0) process_data(net_buf, received);


	for (int i = 0; i < SCREEN_WIDTH; ++i)
	{
		for (int j = 0; j < SCREEN_HEIGHT; ++j)
		{
			int tile_x = i + g_left_x;
			int tile_y = j + g_top_y;
			if ((tile_x < 0) || (tile_y < 0)) continue;

			if (g_Map[tile_y][tile_x] == true)
			{
				origin_tile.a_move(TILE_WIDTH * i, TILE_WIDTH * j);
				origin_tile.a_draw();
			}
			else if (g_Map[tile_y][tile_x] == false)
			{
				block_tile.a_move(TILE_WIDTH * i, TILE_WIDTH * j);
				block_tile.a_draw();
			}


		}
	}

	char buf[100];

	//플레이어 위치 표시
	sf::Text text;
	text.setFont(g_font);

	sprintf_s(buf, "(%d, %d)", avatar.m_x, avatar.m_y);
	text.setString(buf);
	text.setPosition(0, 80);
	text.setCharacterSize(40);
	text.setFillColor(sf::Color::Black);
	text.setStyle(sf::Text::Bold);
	g_window->draw(text);

	//플레이어 HP 표시
	sf::Text player_hp;
	player_hp.setFont(g_font);

	sprintf_s(buf, "HP: %d", avatar.hp);
	player_hp.setString(buf);
	player_hp.setPosition(300, 80);
	player_hp.setCharacterSize(40);
	player_hp.setFillColor(sf::Color::Red);
	player_hp.setStyle(sf::Text::Bold);
	g_window->draw(player_hp);



	//플레이어 레벨 표시
	sf::Text player_level;
	player_level.setFont(g_font);

	sprintf_s(buf, "Level: %d", avatar.level);
	player_level.setString(buf);
	player_level.setPosition(600, 80);
	player_level.setCharacterSize(40);
	player_level.setFillColor(sf::Color::Black);
	player_level.setStyle(sf::Text::Bold);
	g_window->draw(player_level);

	//플레이어 Exp 표시
	sf::Text player_exp;
	player_exp.setFont(g_font);

	sprintf_s(buf, "Exp: %d", avatar.exp);
	player_exp.setString(buf);
	player_exp.setPosition(900, 80);
	player_exp.setCharacterSize(40);
	player_exp.setStyle(sf::Text::Bold);
	g_window->draw(player_exp);

	showChat();


	avatar.draw();
	for (auto& pl : players) pl.second.draw();
	for (auto& pl : npcs) pl.second.draw();
}

void send_move_packet(int dir)
{
	cs_packet_move packet;
	packet.size = sizeof(packet);
	packet.type = CS_MOVE;
	packet.direction = dir;
	size_t sent = 0;
	socket.send(&packet, sizeof(packet), sent);
}

void send_login_packet()
{
	cs_packet_login packet;
	packet.size = sizeof(packet);
	packet.type = CS_LOGIN;
	string name = "PL";
	auto tt = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
	name += to_string(tt % 1000);

	int p_id;
	cout << "id를 입력하세요: ";
	scanf_s("%d", &p_id);

	packet.id = p_id;

	
	strcpy_s(packet.player_id, name.c_str());


	size_t sent = 0;
	socket.send(&packet, sizeof(packet), sent);
} //

void send_attack_packet()
{

	cs_packet_attack packet;
	packet.size = sizeof(packet);
	packet.type = CS_ATTACK;
	size_t sent = 0;
	socket.send(&packet, sizeof(packet), sent);
}



void init_map()
{
	char data;
	FILE* fp = fopen("Map_Data.txt", "rb");

	int count = 0;
	int i = 0, j = 0;


	while (fscanf_s(fp, "%c", &data) != EOF) {
		//printf("%c", data);

		switch (data)
		{
		case '0':
			if (j < WORLD_WIDTH)
			{
				g_Map[i][j] = true;
				++j;
			}
			else
			{
				j = 0;
				++i;
				g_Map[i][j] = true;
				++j;

				//printf("%d\n", i);
			}
			break;
		case '1':
			if (j < WORLD_WIDTH)
			{
				g_Map[i][j] = false;
				++j;
			}
			else
			{
				j = 0;
				++i;
				g_Map[i][j] = false;
				++j;

				//printf("%d\n", i);

			}
			break;
		default:
			break;
		}
	}
}


int main()
{

	wcout.imbue(locale("korean"));

	cout << "IP를 입력하세요: ";
	char ip[50];
	scanf_s("%s", ip, 50);

	sf::Socket::Status status = socket.connect(ip, SERVER_PORT);

	socket.setBlocking(false);

	if (status != sf::Socket::Done) {
		wcout << L"서버와 연결할 수 없습니다.\n";
		while (true);
	}

	init_map();
	client_initialize();

	send_login_packet();

	sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "2D CLIENT");
	g_window = &window;


	while (window.isOpen())
	{
		sf::Event event;
		while (window.pollEvent(event))
		{
			if (event.type == sf::Event::Closed)
				window.close();


			if (event.type == sf::Event::KeyPressed) {
				int dir = -1;

				switch (event.key.code) {
				case sf::Keyboard::Left:
					dir = 3;
					break;
				case sf::Keyboard::Right:
					dir = 1;
					break;
				case sf::Keyboard::Up:
					dir = 0;
					break;
				case sf::Keyboard::Down:
					dir = 2;
					break;
				case sf::Keyboard::Enter:
				{
					char message[MAX_STR_LEN] = {};

					if (chatting_func == false)
						chat_thread = new thread{ Chatting };
				}
				break;
				case sf::Keyboard::A:
				{
					send_attack_packet();
				}
				break;
				case sf::Keyboard::Escape:
					window.close();
					break;

				}
				if (-1 != dir) send_move_packet(dir);

			}

		}


		window.clear();
		client_main();
		window.display();
	}
	client_finish();

	return 0;
}