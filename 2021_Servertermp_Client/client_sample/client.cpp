#define SFML_STATIC 1
#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <chrono>
using namespace std;

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

#include "C:\Users\최주은\source\repos\2021_Servertermp\2021_GameServer\protocol.h"

sf::TcpSocket socket;

constexpr auto SCREEN_WIDTH = 20;
constexpr auto SCREEN_HEIGHT = 20;

constexpr auto TILE_WIDTH = 65;
constexpr auto WINDOW_WIDTH = TILE_WIDTH * SCREEN_WIDTH + 10;   // size of window
constexpr auto WINDOW_HEIGHT = TILE_WIDTH * SCREEN_WIDTH + 10;
constexpr auto BUF_SIZE = MAX_BUFFER;

int g_left_x;
int g_top_y;
int g_myid;

sf::RenderWindow* g_window;
sf::Font g_font;

class OBJECT {
private:
	bool m_showing;
	sf::Sprite m_sprite;
	sf::Text m_name;
	sf::Text m_level;
	sf::Text m_hp;
	chrono::system_clock::time_point m_mess_end_time;
	sf::Text m_chat;

public:
	int m_x, m_y;
	char name[MAX_ID_LEN];
	int level;
	int hp;
	int exp;
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
	
};

OBJECT avatar;
unordered_map <int, OBJECT> players;
unordered_map <int, OBJECT> npcs;

OBJECT origin_tile;
OBJECT block_tile;

sf::Texture* board;
sf::Texture* block;
sf::Texture* pieces;

bool g_Map[WORLD_WIDTH][WORLD_HEIGHT];

void client_initialize()
{
	board = new sf::Texture;
	block = new sf::Texture;
	pieces = new sf::Texture;

	if (false == g_font.loadFromFile("cour.ttf")) {
		cout << "Font Loading Error!\n";
		while (true);
	}

	board->loadFromFile("wheat.bmp");
	block->loadFromFile("block.bmp");
	pieces->loadFromFile("chess2.png");

	origin_tile = OBJECT{ *board, 0, 0, TILE_WIDTH, TILE_WIDTH };
	block_tile = OBJECT{ *block, 0, 0, TILE_WIDTH, TILE_WIDTH };
	avatar = OBJECT{ *pieces, 128, 0, 64, 64 };
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

		char buf[20];

		sprintf(buf, "Hp : %d", packet->HP);
		avatar.set_hp(buf);

		sprintf(buf, "Level : %d", packet->LEVEL);
		avatar.set_level(buf);


		//리스폰

		avatar.show();
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
		break;
	}
	case SC_ADD_OBJECT:
	{
		sc_packet_add_object* my_packet = reinterpret_cast<sc_packet_add_object*>(ptr);
		int id = my_packet->id;

		char buf[20];


	
		if (my_packet->obj_class == 0) {
			players[id] = OBJECT{ *pieces, 64, 0, 64, 64 };
			players[id].move(my_packet->x, my_packet->y);
			players[id].show();

			sprintf(buf, "Hp : %d", my_packet->HP);
			players[id].set_hp(buf);

			sprintf(buf, "Level : %d", my_packet->LEVEL);
			players[id].set_level(buf);

			players[id].set_name(my_packet->name);
		}
		else {
			npcs[id] = OBJECT{ *pieces, 0, 0, 64, 64 };
			npcs[id].move(my_packet->x, my_packet->y);
			npcs[id].show();
			sprintf(buf, "Hp : %d", my_packet->HP);
			npcs[id].set_hp(buf);

			sprintf(buf, "Level : %d", my_packet->LEVEL);
			npcs[id].set_level(buf);
			npcs[id].set_name(my_packet->name);
		}
		
		break;
	}
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

	avatar.set_name(name.c_str());
	strcpy_s(packet.player_id, name.c_str());

	size_t sent = 0;
	socket.send(&packet, sizeof(packet), sent);
} //

void init_map()
{
	char data;
	FILE* fp = fopen("Map_Data.txt", "rb");

	int count = 0;
	int i = 0, j = 0;


	while (fscanf(fp, "%c", &data) != EOF) {
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
	sf::Socket::Status status = socket.connect("127.0.0.1", SERVER_PORT);

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