// velocityFromRotation() can be called like a plain function.
const VelocityFromRotation =
  Phaser.Physics.Arcade.ArcadePhysics.prototype.velocityFromRotation;
const setSpeed = Phaser.Physics.Arcade.ArcadePhysics;

const config = {
  type: Phaser.AUTO,
  width: 752,
  height: 400,
  parent: "phaser-example",
  physics: {
    default: "arcade",
    arcade: { debug: false },
  },
  scene: {
    preload: preload,
    create: create,
    update: update,
  },
};

const game = new Phaser.Game(config);

class Racecar extends Phaser.Physics.Arcade.Image {
  throttle = 0;

  configure() {
    this.angle = 0;
    this.body.angularDrag = 120;
    this.body.maxSpeed = 1024;
    this.body.setSize(64, 64, true);
  }

  update(delta, cursorKeys, speed) {
    this.body.setVelocityX(20 * speed);
  }
}

function preload() {
  //this.load.setBaseURL("http://127.0.0.1:5500"); //I think you comment this out if you ARE running live server??
  // Load the background image (752x501) and car spritesheet
  this.load.image("soil", "assets/road.webp");
  this.load.spritesheet("car", "assets/spr_red_coupe_0.png", {
    frameWidth: 96,
    frameHeight: 64,
  });
}

function create() {
  // Create a background that fills the game canvas.
  this.ground = this.add
    .tileSprite(
      this.cameras.main.width / 2,
      this.cameras.main.height / 2,
      this.cameras.main.width,
      this.cameras.main.height + 200,
      "soil"
    )
    .setScrollFactor(0, 0);

  // Create the racecar. (Its starting position may be adjusted if needed.)
  this.car = new Racecar(this, 256, 512, "car", 5);
  this.add.existing(this.car);
  this.physics.add.existing(this.car);
  this.car.configure();

  this.car.setOrigin(0.5, 0.65); //New origin to make rotation look ok

  this.cursorKeys = this.input.keyboard.createCursorKeys();

  // Set the camera to follow the car with an offset.
  // A negative y offset moves the car lower on screen.
  this.cameras.main.startFollow(this.car, true, 0.5, 0, 0, 150);

  //Some math to give  safe rotation zone for slope tilt
  const maxRad = Phaser.Math.DegToRad(MAX_TILT_DEG);
  const safeZoom = 1 / (Math.cos(maxRad) * 0.98); //Add extra margin for rotation
  this.cameras.main.setZoom(safeZoom);

  //Websockets
  socket = new WebSocket("ws://192.168.4.1:8765/ws"); // Change to your WebSocket server URL

  socket.onopen = function () {
    console.log("WebSocket connected.");
  };

  socket.onmessage = function (event) {
    // console.log(event);
    if (event.data) {
      const byteArray = event.data.split(" ").map((byte) => parseInt(byte, 16));
      handleWebSocketBytes(byteArray);
    }
  };

  socket.onerror = function (error) {
    console.error("WebSocket Error: ", error);
  };
}

let throttle = 0;
let brake = 0;
let currentSpeed = 5;

// Slope and tilt variables
let targetTiltDeg = 0;    // from websocket
let currentTiltDeg = 0;   // curr
const MAX_TILT_DEG = 12.7;  // limit to avoid craziness

//slope to tan degrees
function slopePctToDeg(pct) {
  return Math.atan(pct / 100) * (180 / Math.PI);
}

function update(time, delta) {
  const { scrollX } = this.cameras.main;
  // Adjust the tile sprite's position so it scrolls with the camera
  this.ground.setTilePosition(scrollX);
  this.car.update(delta, this.cursorKeys, currentSpeed);

  const t = Math.min(delta / 200, 1);         // frame smoothing for slope/tilt
  currentTiltDeg = Phaser.Math.Linear(currentTiltDeg, targetTiltDeg, t);

  // Apply world tilt: rotate camera by +tilt, car by -tilt to keep car upright (camera tilt is all objects)
  const r = Phaser.Math.DegToRad(currentTiltDeg);
  this.cameras.main.setRotation(r);
  //this.car.setRotation(-r);
}

function updateGear(gearValue) {
  const gearElement = document.getElementById("gearIndicator");
  if (!gearElement) return;

  const displayGear = gearValue === 255 ? "R" : gearValue;
  // Update the gear display
  gearElement.textContent = `Gear: ${displayGear}`;
}

function updateMessage(stringVal) {
  const messageElement = document.getElementById("messageDisplay");
  if (!messageElement) return;

  messageElement.textContent = `Message: ${stringVal}`;
}

function handleWebSocketBytes(byteArray) {
  if (byteArray.length !== 16) {
    console.error("Invalid data length. Expected 16 bytes.");
    return;
  }

  console.log(byteArray);

  //Map bytes to vars
  const throttle = byteArray[0];
  const brake = byteArray[1];
  const gear = byteArray[2];
  const speed = Math.round((byteArray[3] * 3.6) / 5); // Convert m/s to km/h
  const reverse = byteArray[4];
  const rpm = byteArray[5] * 100;
  const driveMode = byteArray[6];
  const slopePercentage = (byteArray[7] / 100).toFixed(2) * -1; // Convert slope to percentage
  const message = byteArray
  .slice(8)
  .map((b) => (b === 0 ? "" : String.fromCharCode(b)))
  .join(""); 

  let newSpeed = speed;
  if (reverse) {
    newSpeed = -1 * newSpeed;
  }

  //Read slope deg from WS
  let tiltDeg = slopePercentage;

  //had to apply a clamp to make things look rational
  tiltDeg = Phaser.Math.Clamp(tiltDeg, -MAX_TILT_DEG, MAX_TILT_DEG);
  targetTiltDeg = tiltDeg;

  updateThrottle(throttle);
  updateBrake(brake);
  updateGear(gear);
  updateDriveMode(driveMode);
  updateMessage(message);
  animateSpeedometer(currentSpeed, newSpeed, 100);

  currentSpeed = newSpeed;
}

function handleWebSocketCommand(data) {
  console.log(data);

  let newSpeed = data.speed;
  if (data.reverse) {
    newSpeed = -1 * newSpeed;
  }
  const newThrottle = data.throttle;
  const newBrake = data.brake;

  //Read slope deg from WS
  let tiltDeg = data.Slope_Percentage;

  //had to apply a clamp to make things look rational
  tiltDeg = Phaser.Math.Clamp(tiltDeg, -MAX_TILT_DEG, MAX_TILT_DEG);
  targetTiltDeg = tiltDeg;

  //Read drive mode
  const driveMode = data.Drive_Mode;
  updateDriveMode(driveMode);

  const gear = data.gear;
  updateGear(gear);

  const message = data.Message;
  updateMessage(message);

  animateSpeedometer(currentSpeed, newSpeed, 100);
  updateThrottle(newThrottle);
  updateBrake(newBrake);

  currentSpeed = newSpeed;

}

function updateDriveMode(modeValue) {
  const el = document.getElementById("driveModeIndicator");
  if (!el) return;

  const v = Number(modeValue);

  switch (v) {
    case 0:
      el.textContent = "Dual Pedal";                  // show nothing for Drive mode 0
      el.title = "Drive Mode: Normal";
      break;
    case 1:
      el.textContent = "Single Pedal";      // show label for Drive mode 1
      el.title = "Drive Mode: Single Pedal";
      break;  
    default:
      el.textContent = "";
      el.title = "Drive Mode: Unknown"; //catch
  }
}

// Smoothly animate the speedometer update
function animateSpeedometer(fromSpeed, toSpeed, duration) {
  const meterBar = document.getElementById("meter-bg-bar");
  const speedDisplay = document.getElementById("speed");
  const startTime = performance.now();
  const range = toSpeed - fromSpeed;

  function update(now) {
    const elapsed = now - startTime;
    const progress = Math.min(elapsed / duration, 1); // Clamp progress at 1
    const newSpeed = fromSpeed + range * progress;
    // Calculate the stroke-dashoffset based on newSpeed
    // 615 corresponds to 0 km/h and 0 corresponds to 180 km/h
    const strokeDashoffset = 615 - (newSpeed / 180) * 615;

    meterBar.style.strokeDashoffset = strokeDashoffset;
    speedDisplay.textContent = Math.round(newSpeed);

    if (progress < 1) {
      requestAnimationFrame(update);
    } else {
      // Once done, update the currentSpeed to the final value
      currentSpeed = toSpeed;
    }
  }

  requestAnimationFrame(update);
}

function updateThrottle(newThrottle) {
  if (newThrottle <= 100) {
    document.getElementById("throttleBar").style.width = newThrottle + "%";
    document.getElementById("throttleBar").textContent = newThrottle + "%";
  }
}

function updateBrake(newBrake) {
  if (newBrake <= 100) {
    document.getElementById("brakeBar").style.width = newBrake + "%";
    document.getElementById("brakeBar").textContent = newBrake + "%";
  }
}
