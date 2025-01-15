<script lang="ts">
  import * as Tabs from "$lib/components/ui/tabs";
  import GeneralEndpointConfig from "./general-endpoint-config.svelte";
  import ParametersConfig from "./parameters-config.svelte";
  import QueryConfig from "./query-config.svelte";
  import type { EndpointConfig } from "$lib/types";

  const { config, onUpdate } = $props<{
    config: EndpointConfig;
    onUpdate: (config: EndpointConfig) => void;
  }>();

  let activeTab = $state('general');

  function handleGeneralUpdate(values: Partial<EndpointConfig>) {
    onUpdate({ ...config, ...values });
  }

  function handleParametersUpdate(parameters: EndpointConfig['parameters']) {
    onUpdate({ ...config, parameters });
  }

  function handleQueryUpdate(query: EndpointConfig['query']) {
    onUpdate({ ...config, query });
  }
</script>

<Tabs.Root value={activeTab} onValueChange={(value) => activeTab = value}>
  <Tabs.List>
    <Tabs.Trigger value="general">General</Tabs.Trigger>
    <Tabs.Trigger value="parameters">Parameters</Tabs.Trigger>
    <Tabs.Trigger value="query">Query</Tabs.Trigger>
  </Tabs.List>

  <Tabs.Content value="general">
    <GeneralEndpointConfig
      path={config.path}
      method={config.method}
      version={config.version}
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