<script lang="ts">
  import * as Tabs from "$lib/components/ui/tabs";
  import GeneralEndpointConfig from "./general-endpoint-config.svelte";
  import ParametersConfig from "./parameters-config.svelte";
  import QueryConfig from "./query-config.svelte";
  import type { EndpointConfig } from "$lib/types/config";

  export let config: EndpointConfig;
  export let onUpdate: (config: EndpointConfig) => void;

  let activeTab = "general";

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

<Tabs.Root value={activeTab}>
  <Tabs.List>
    <Tabs.Trigger value="general">General</Tabs.Trigger>
    <Tabs.Trigger value="parameters">Parameters</Tabs.Trigger>
    <Tabs.Trigger value="query">Query</Tabs.Trigger>
  </Tabs.List>

  <Tabs.Content value="general">
    <GeneralEndpointConfig
      path={config.path}
      method={config.method}
      description={config.description}
      onUpdate={handleGeneralUpdate}
    />
  </Tabs.Content>

  <Tabs.Content value="parameters">
    <ParametersConfig
      parameters={config.parameters}
      onUpdate={handleParametersUpdate}
    />
  </Tabs.Content>

  <Tabs.Content value="query">
    <QueryConfig
      query={config.query}
      onUpdate={handleQueryUpdate}
    />
  </Tabs.Content>
</Tabs.Root> 