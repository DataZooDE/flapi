import React from 'react';
import styles from './styles.module.css';

export default function Logo({ width = 35, height = 35 }) {
  return (
    <svg
      width={width}
      height={height}
      viewBox="0 0 100 100"
      className={styles.logo}
    >
      {/* Bird body */}
      <circle cx="60" cy="50" r="35" fill="#FFE082" />
      
      {/* Wing */}
      <path
        d="M 45 50 Q 35 40, 25 50 T 45 50"
        fill="#FFD54F"
        className={styles.wing}
      />
      
      {/* Eye */}
      <circle cx="75" cy="40" r="8" fill="#263238" />
      <circle cx="77" cy="38" r="3" fill="#ffffff" />
      
      {/* Beak */}
      <path
        d="M 85 50 L 100 45 L 85 55 Z"
        fill="#FF7043"
      />
    </svg>
  );
} 