import React, { useEffect, useRef } from 'react';
import styles from './styles.module.css';

export default function GameBackground() {
  const canvasRef = useRef(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    const ctx = canvas.getContext('2d');
    let animationFrameId;

    // Bird properties
    const bird = {
      x: 0,
      y: 0,
      velocity: 0,
      gravity: 0.4,
      jump: -8,
      size: 35,
      targetY: 0
    };

    // Pipe properties
    let pipes = [];
    const pipeWidth = 60;
    const pipeGap = 180;
    const pipeSpeed = 3;

    // City properties
    let buildings = [];
    const buildingSpeed = 1;
    const buildingColors = ['#2B4865', '#256D85', '#1B4242', '#183D3D'];

    // Cloud properties
    let clouds = [];
    const cloudSpeed = 0.6;
    const maxClouds = 8;

    function resizeCanvas() {
      canvas.width = canvas.offsetWidth;
      canvas.height = canvas.offsetHeight;
      
      // Reset bird position after resize
      bird.x = canvas.width * 0.2;  // 20% from left
      bird.y = canvas.height * 0.4;  // 40% from top
      bird.targetY = bird.y;
      
      // Clear existing pipes and create new ones
      pipes = [];
      createPipe();
    }

    function createCloud() {
      const size = 20 + Math.random() * 40;
      const yPosition = Math.random() * (canvas.height * 0.4);
      clouds.push({
        x: canvas.width,
        y: yPosition,
        size: size,
        speed: cloudSpeed * (0.7 + Math.random() * 0.6),
        opacity: 0.4 + Math.random() * 0.4
      });
    }

    function createBuilding() {
      const height = 50 + Math.random() * 150;
      buildings.push({
        x: canvas.width,
        height: height,
        width: 40 + Math.random() * 60,
        color: buildingColors[Math.floor(Math.random() * buildingColors.length)],
        windows: []
      });
    }

    function createPipe() {
      // Ensure pipe gap is within visible area
      const minGapY = canvas.height * 0.2;
      const maxGapY = canvas.height * 0.6;
      const gapY = minGapY + Math.random() * (maxGapY - minGapY);
      
      pipes.push({
        x: canvas.width,
        gapY: gapY,
        passed: false
      });
    }

    function drawCloud(cloud) {
      ctx.fillStyle = `rgba(255, 255, 255, ${cloud.opacity})`;
      ctx.beginPath();
      ctx.arc(cloud.x, cloud.y, cloud.size, 0, Math.PI * 2);
      ctx.fill();
      
      const puffCount = 3;
      for (let i = 0; i < puffCount; i++) {
        const puffSize = cloud.size * (0.6 + Math.random() * 0.4);
        const puffX = cloud.x + (i - 1) * cloud.size * 0.8;
        const puffY = cloud.y - cloud.size * 0.2 + Math.random() * cloud.size * 0.4;
        ctx.beginPath();
        ctx.arc(puffX, puffY, puffSize, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    function drawBuilding(building) {
      ctx.fillStyle = building.color;
      ctx.fillRect(building.x, canvas.height - building.height, building.width, building.height);
      
      ctx.fillStyle = 'rgba(255, 255, 150, 0.3)';
      const windowSize = 8;
      const windowGap = 15;
      const windowRows = Math.floor((building.height - 20) / windowGap);
      const windowCols = Math.floor((building.width - 10) / windowGap);
      
      for(let row = 0; row < windowRows; row++) {
        for(let col = 0; col < windowCols; col++) {
          if(Math.random() > 0.3) {
            ctx.fillRect(
              building.x + 10 + col * windowGap,
              canvas.height - building.height + 10 + row * windowGap,
              windowSize,
              windowSize
            );
          }
        }
      }
    }

    function drawBird() {
      // Bird shadow
      ctx.fillStyle = 'rgba(0, 0, 0, 0.2)';
      ctx.beginPath();
      ctx.arc(bird.x + 5, bird.y + 5, bird.size, 0, Math.PI * 2);
      ctx.fill();

      // Bird body
      ctx.fillStyle = '#FED34E';
      ctx.beginPath();
      ctx.arc(bird.x, bird.y, bird.size, 0, Math.PI * 2);
      ctx.fill();
      
      // Wing animation with larger size
      const wingOffset = (Date.now() % 300) > 150 ? 8 : -8;  // Increased offset
      ctx.beginPath();
      ctx.fillStyle = '#FFE082';
      ctx.arc(bird.x - bird.size * 0.75, bird.y + wingOffset, bird.size * 0.7, 0, Math.PI * 2);  // Increased wing size
      ctx.fill();

      // Larger eye
      ctx.fillStyle = '#333';
      ctx.beginPath();
      ctx.arc(bird.x + bird.size * 0.3, bird.y - bird.size * 0.2, bird.size * 0.25, 0, Math.PI * 2);
      ctx.fill();
      
      // Larger eye highlight
      ctx.fillStyle = '#FFF';
      ctx.beginPath();
      ctx.arc(bird.x + bird.size * 0.4, bird.y - bird.size * 0.3, bird.size * 0.15, 0, Math.PI * 2);
      ctx.fill();

      // Larger beak
      ctx.fillStyle = '#FF7043';
      ctx.beginPath();
      ctx.moveTo(bird.x + bird.size * 0.8, bird.y);
      ctx.lineTo(bird.x + bird.size * 1.5, bird.y - bird.size * 0.2);
      ctx.lineTo(bird.x + bird.size * 0.8, bird.y + bird.size * 0.2);
      ctx.closePath();
      ctx.fill();

      // Optional: Add outline for better contrast
      ctx.strokeStyle = '#333';
      ctx.lineWidth = 3;
      ctx.beginPath();
      ctx.arc(bird.x, bird.y, bird.size, 0, Math.PI * 2);
      ctx.stroke();
    }

    function updateBirdPosition() {
      const nextPipe = pipes.find(pipe => pipe.x + pipeWidth > bird.x);
      if (nextPipe) {
        bird.targetY = nextPipe.gapY + pipeGap / 2;
        
        const diff = bird.targetY - bird.y;
        if (Math.abs(diff) > 5) {
          bird.velocity = diff * 0.1;
        }
      } else {
        bird.velocity *= 0.95;
      }

      bird.y += bird.velocity;
      bird.velocity = Math.max(Math.min(bird.velocity, 8), -8);

      // Keep bird within bounds
      const minY = bird.size;
      const maxY = canvas.height - bird.size;
      
      if (bird.y > maxY) {
        bird.y = maxY;
        bird.velocity = bird.jump * 0.5;
      }
      if (bird.y < minY) {
        bird.y = minY;
        bird.velocity = 0;
      }
    }

    function updateGame() {
      if (!canvas.width || !canvas.height) {
        resizeCanvas();
        return;
      }

      ctx.clearRect(0, 0, canvas.width, canvas.height);

      // Create new elements
      if (buildings.length === 0 || buildings[buildings.length - 1].x < canvas.width - 100) {
        createBuilding();
      }
      if (clouds.length < maxClouds && Math.random() < 0.02) {
        createCloud();
      }
      if (pipes.length === 0 || pipes[pipes.length - 1].x < canvas.width - 300) {
        createPipe();
      }

      // Update elements
      buildings = buildings.filter(building => {
        building.x -= buildingSpeed;
        return building.x + building.width > 0;
      });

      clouds = clouds.filter(cloud => {
        cloud.x -= cloud.speed;
        return cloud.x + cloud.size * 3 > 0;
      });

      pipes = pipes.filter(pipe => {
        pipe.x -= pipeSpeed;
        return pipe.x > -pipeWidth;
      });

      updateBirdPosition();

      // Draw everything
      clouds.forEach(drawCloud);
      buildings.forEach(drawBuilding);
      drawPipes();
      drawBird();

      animationFrameId = requestAnimationFrame(updateGame);
    }

    function drawPipes() {
      ctx.fillStyle = '#74BF2E';
      pipes.forEach(pipe => {
        ctx.fillRect(pipe.x, 0, pipeWidth, pipe.gapY);
        ctx.fillRect(pipe.x - 5, pipe.gapY - 20, pipeWidth + 10, 20);
        
        ctx.fillRect(pipe.x, pipe.gapY + pipeGap, pipeWidth, canvas.height);
        ctx.fillRect(pipe.x - 5, pipe.gapY + pipeGap, pipeWidth + 10, 20);
      });
    }

    // Initial setup
    window.addEventListener('resize', resizeCanvas);
    resizeCanvas();
    updateGame();

    return () => {
      window.removeEventListener('resize', resizeCanvas);
      cancelAnimationFrame(animationFrameId);
    };
  }, []);

  return (
    <canvas 
      ref={canvasRef} 
      className={styles.gameCanvas}
    />
  );
} 