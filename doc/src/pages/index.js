import React from 'react';
import clsx from 'clsx';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import HomepageFeatures from '@site/src/components/HomepageFeatures';
import GameBackground from '@site/src/components/GameBackground';
import styles from './index.module.css';

function HomepageHeader() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <header className={clsx('hero hero--primary', styles.heroBanner)}>
      <GameBackground />
      <div className="container">
        <h1 className="hero__title">Instant SQL based APIs</h1>
        <p className="hero__subtitle">
          Transform your SQL queries into production-ready APIs in minutes
        </p>
        <div className={styles.buttons}>
          <Link
            className="button button--secondary button--lg"
            to="/docs/getting-started/introduction">
            Get Started - 5min ⏱️
          </Link>
        </div>
      </div>
    </header>
  );
}

function OverviewSection() {
  return (
    <section className={styles.overview}>
      <div className="container">
        <div className={styles.overviewContent}>
          <h2 className={styles.overviewTitle}>How it Works</h2>
          <img 
            src="https://i.imgur.com/m7UVZlR.png" 
            alt="flAPI Overview"
            className={styles.overviewImage}
          />
          <div className={styles.overviewText}>
            <p>
              flAPI connects your data sources to REST APIs in three simple steps:
            </p>
            <ol>
              <p><strong>1. Connect</strong> - Link your data sources (PostgreSQL, BigQuery, SAP, etc.)</p>
              <p><strong>2. Project</strong> - Define SQL templates with parameters and caching</p>
              <p><strong>3. Serve</strong> - Get instant REST APIs with built-in features</p>
            </ol>
          </div>
        </div>
      </div>
    </section>
  );
}

export default function Home() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout
      title={`${siteConfig.title}`}
      description="Transform your SQL queries into production-ready APIs in minutes">
      <HomepageHeader />
      <main>
        <OverviewSection />
        <HomepageFeatures />
      </main>
    </Layout>
  );
} 