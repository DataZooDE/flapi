<script lang="ts">
  import { TabGroup, TabList, Tab, TabPanel } from "$lib/components/ui/tabs";
  import GeneralEndpointConfig from "./general-endpoint-config.svelte";
  import ParametersConfig from "./parameters-config.svelte";
  import QueryConfig from "./query-config.svelte";
  import type { EndpointConfig } from "$lib/types/config";

  export let config: EndpointConfig;
  export let onUpdate: (config: EndpointConfig) => void;

  let activeTab = 0;

  function handleGeneralUpdate(generalConfig: Partial<EndpointConfig>) {
    config = { ...config, ...generalConfig };
    onUpdate(config);
  }

  function handleParametersUpdate(parameters: EndpointConfig["parameters"]) {
    config.parameters = parameters;
    onUpdate(config);
  }

  function handleQueryUpdate(query: EndpointConfig["query"]) {
    config.query = query;
    onUpdate(config);
  }
</script>

<TabGroup bind:selected={activeTab}>
  <TabList>
    <Tab value={0}>General</Tab>
    <Tab value={1}>Parameters</Tab>
    <Tab value={2}>Query</Tab>
  </TabList>

  <TabPanel value={0}>
    <GeneralEndpointConfig
      path={config.path}
      method={config.method}
      description={config.description}
      onUpdate={handleGeneralUpdate}
    />
  </TabPanel>

  <TabPanel value={1}>
    <ParametersConfig
      parameters={config.parameters}
      onUpdate={handleParametersUpdate}
    />
  </TabPanel>

  <TabPanel value={2}>
    <QueryConfig
      query={config.query}
      onUpdate={handleQueryUpdate}
    />
  </TabPanel>
</TabGroup> 