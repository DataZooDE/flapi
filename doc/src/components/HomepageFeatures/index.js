import React, { useState } from 'react';
import clsx from 'clsx';
import styles from './styles.module.css';

const formatCode = (code, language) => {
  // Basic syntax highlighting
  if (language === 'sql') {
    return code
      .replace(/(SELECT|FROM|WHERE|AND|OR|GROUP BY|ORDER BY|LIMIT|CREATE|TABLE|AS)/g, '<span class="keyword">$1</span>')
      .replace(/'([^']+)'/g, '<span class="string">\'$1\'</span>')
      .replace(/--[^\n]*/g, '<span class="comment">$&</span>')
      .replace(/\b(\d+)\b/g, '<span class="number">$1</span>');
  }
  if (language === 'yaml') {
    return code
      .replace(/^([^:]+):/gm, '<span class="keyword">$1:</span>')
      .replace(/'([^']+)'/g, '<span class="string">\'$1\'</span>')
      .replace(/#[^\n]*/g, '<span class="comment">$&</span>');
  }
  return code;
};

const CodeTabs = ({ examples }) => {
  const [activeTab, setActiveTab] = useState(0);

  return (
    <div className={styles.codeTabs}>
      <div className={styles.tabList}>
        {examples.map((example, idx) => (
          <button
            key={idx}
            className={clsx(styles.tabButton, activeTab === idx && styles.activeTab)}
            onClick={() => setActiveTab(idx)}
          >
            {example.label}
          </button>
        ))}
      </div>
      <div className={styles.codeExample}>
        <pre dangerouslySetInnerHTML={{ 
          __html: formatCode(examples[activeTab].code, examples[activeTab].language) 
        }} />
      </div>
    </div>
  );
};

const FeatureList = [
  {
    title: 'Easy as Flying',
    description: (
      <>
        Create APIs with the simplicity of a bird in flight. Just write SQL templates 
        and let flAPI handle the rest. No complex setup, no heavy frameworks - 
        just pure, straightforward API creation.
      </>
    ),
    background: 'var(--flappy-sky)',
    codeExamples: [
      {
        label: 'SQL Template',
        language: 'sql',
        code: `-- Define your API query
SELECT * FROM users
WHERE 1=1
{{#params.id}}
  AND id = {{{params.id}}}
{{/params.id}}
{{#params.role}}
  AND role = '{{{params.role}}}'
{{/params.role}}`
      },
      {
        label: 'Config',
        language: 'yaml',
        code: `# users.yaml
url-path: /users/
request:
  - field-name: id
    field-in: query
    validators:
      - type: int
  - field-name: role
    field-in: query
    validators:
      - type: string`
      },
      {
        label: 'API Call',
        language: 'bash',
        code: `# Instant REST API
curl 'http://localhost:8080/users?id=123&role=admin'`
      }
    ]
  },
  {
    title: 'Soar Through Data Sources',
    description: (
      <>
        Connect to BigQuery, SAP, Parquet, Iceberg, Postgres, MySQL and more. 
        Your data takes flight with DuckDB's powerful wings, making integration 
        as smooth as a bird gliding through clouds.
      </>
    ),
    background: 'var(--flappy-white)',
    codeExamples: [
      {
        label: 'BigQuery',
        language: 'yaml',
        code: `connections:
  bigquery:
    init: |
      INSTALL 'bigquery';
      LOAD 'bigquery';
    properties:
      project_id: 'my-project'`
      },
      {
        label: 'PostgreSQL',
        language: 'yaml',
        code: `connections:
  postgres:
    init: |
      INSTALL postgres;
      LOAD postgres;
    properties:
      host: localhost
      port: 5432
      database: mydb`
      },
      {
        label: 'Parquet',
        language: 'yaml',
        code: `connections:
  parquet:
    properties:
      path: './data/*.parquet'`
      }
    ]
  },
  {
    title: 'Nest Your Data Safely',
    description: (
      <>
        Built-in caching and security features keep your data safe and fast, 
        like a well-protected bird's nest. With row-level security, rate limiting, 
        and intelligent caching, your data stays secure and performs at its best.
      </>
    ),
    background: 'var(--flappy-sky)',
    codeExamples: [
      {
        label: 'Security',
        language: 'sql',
        code: `# Row-level security
SELECT * FROM data
WHERE 1=1
{{#context.user.roles.admin}}
  -- Admins see all data
{{/context.user.roles.admin}}
{{^context.user.roles.admin}}
  AND department = '{{{context.user.department}}}'
{{/context.user.roles.admin}}`
      },
      {
        label: 'Caching',
        language: 'yaml',
        code: `cache:
  cache-table-name: 'products_cache'
  cache-source: products_cache.sql
  refresh-time: 15m`
      },
      {
        label: 'Rate Limiting',
        language: 'yaml',
        code: `rate-limit:
  enabled: true
  max: 100        # requests
  interval: 60    # seconds
  rules:
    - user: admin
      max: 1000`
      }
    ]
  },
  {
    title: 'Deploy Anywhere',
    description: (
      <>
        Take flight with a single binary file or tiny Docker image. No dependencies, 
        no complex setup. Deploy on AWS, Azure, GCP, or self-host with the same ease. 
        From local development to cloud-scale production, flAPI adapts to any environment.
      </>
    ),
    background: 'var(--flappy-white)',
    codeExamples: [
      {
        label: 'Docker',
        language: 'bash',
        code: `# Pull and run with Docker
docker pull ghcr.io/datazoode/flapi
docker run -p 8080:8080 -v $(pwd)/config:/config \
  ghcr.io/datazoode/flapi -c /config/flapi.yaml`
      },
      {
        label: 'Binary',
        language: 'bash',
        code: `# Download and run binary
wget https://github.com/datazoode/flapi/releases/latest/flapi
chmod +x flapi
./flapi -c config.yaml`
      },
      {
        label: 'Cloud',
        language: 'yaml',
        code: `# AWS ECS Task Definition
{
  "containerDefinitions": [{
    "name": "flapi",
    "image": "ghcr.io/datazoode/flapi",
    "portMappings": [{"containerPort": 8080}],
    "command": ["-c", "/config/flapi.yaml"]
  }]
}`
      }
    ]
  },
];

function Feature({title, description, background, codeExamples}) {
  return (
    <section 
      className={styles.featureSection}
      style={{ backgroundColor: background }}
    >
      <div className="container">
        <div className={styles.featureContent}>
          <h2 className={styles.featureTitle}>{title}</h2>
          <div className={styles.featureDescription}>{description}</div>
          
          {codeExamples && (
            <CodeTabs examples={codeExamples} />
          )}

          <div className={styles.featurePattern}></div>
        </div>
      </div>
    </section>
  );
}

export default function HomepageFeatures() {
  return (
    <div className={styles.features}>
      {FeatureList.map((props, idx) => (
        <Feature key={idx} {...props} />
      ))}
    </div>
  );
} 