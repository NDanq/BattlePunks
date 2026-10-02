-- Input from hand-written Lua. The entity has an "Input" component reading
-- SandboxInput.cpp's "Default" map; the engine resolves it at the start of
-- TP_PrePhysics, before this script, so what it reads is this frame's.
--
--   Space / pad South   Jump      (an action on the component)
--   Shift / pad R1      Boost
--   F3                  print the mouse position   (the Input library's raw keys)
--   C                   capture or release the mouse
return {
    TickPhase = TickPhase.PrePhysics,

    OnStart = function(self)
        self.Input = self.Entity:GetComponent("Input")
        print("InputDemo: Space jumps (Shift boosts), F3 prints the mouse, C captures it")
    end,

    OnTick = function(self, dt)
        if self.Input:WasPressed("Jump") then
            print("InputDemo: jump" .. (self.Input:IsDown("Boost") and ", boosted" or ""))
        end

        if Input.WasKeyPressed("F3") then
            local x, y = Input.GetMousePosition()
            print("InputDemo: mouse at " .. x .. ", " .. y)
        end

        if Input.WasKeyPressed("C") then
            Input.SetMouseCaptured(not Input.IsMouseCaptured())
            print("InputDemo: mouse " .. (Input.IsMouseCaptured() and "captured" or "released"))
        end
    end,
}
