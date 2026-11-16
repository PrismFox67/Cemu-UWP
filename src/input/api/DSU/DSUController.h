#pragma once

#include "input/api/Controller.h"
#include "input/api/DSU/DSUControllerProvider.h"
#include "Cafe/HW/AI/AI.h"
#include "Cafe/HW/AI/AI.h"
#include "Cafe/HW/AI/AI.h"
#include "Cafe/HW/AI/AI.h"

class DSUController : public Controller<DSUControllerProvider>
{
public:
	DSUController(uint32 index);
	DSUController(uint32 index, const DSUProviderSettings& settings);
	
	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::DSUClient) == "DSUController");
		return to_string(InputAPI::DSUClient);
	}
	InputAPI::Type api() const override { return InputAPI::DSUClient; }

	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	const DSUProviderSettings& get_provider_settings() const { return m_provider->get_settings(); }

	bool connect() override;
	bool is_connected() override;
	// asks the server again (at most once a second) while no data arrives; logs connect/disconnect
	bool PollConnection();

	
	bool has_motion() override { return true; }
	MotionSample get_motion_sample() override;

	bool has_position() override;
	glm::vec2 get_position() override;
	glm::vec2 get_prev_position() override;
	PositionVisibility GetPositionVisibility() override;

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

private:
	uint32 m_index;
	std::atomic<uint64> m_lastRequestTick{0}; // polled from the input thread and the settings page
	std::atomic<bool> m_loggedConnected{false};
};

